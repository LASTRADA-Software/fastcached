// SPDX-License-Identifier: Apache-2.0
#include "NodeIdentity.hpp"
#include "NodeKey.hpp"
#include "NodeStateFiles.hpp"

#include <FastCache/Cluster/FormationRecord.hpp>
#include <FastCache/Consensus/FileRaftStorage.hpp>
#include <FastCache/Core/Owner.hpp>
#include <FastCache/Core/WireFields.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <format>
#include <memory>
#include <optional>
#include <system_error>
#include <utility>

#if defined(_WIN32)
    #include <io.h>
#else
    #include <unistd.h>
#endif

namespace FastCache::Node
{

namespace
{
    /// What every key file opens with. Four bytes that no text file and no other file this
    /// project writes begins with, so a stranger's file is refused as one rather than read
    /// as a damaged key.
    constexpr auto Magic = std::array { std::byte { 'F' }, std::byte { 'C' }, std::byte { 'N' }, std::byte { 'K' } };

    /// The layout this build reads and writes. Moves when the layout does, and a file
    /// carrying another value is `ForeignFormat` -- intact, and not this build's.
    constexpr std::uint16_t FormatVersion = 1;

    /// Bytes in front of the key material: the magic, then the version.
    constexpr std::size_t HeaderBytes = Magic.size() + sizeof(std::uint16_t);

    static_assert(NodeKeyFileBytes == HeaderBytes + Ed25519SeedBytes + Ed25519PublicKeyBytes,
                  "a key file is its header, its seed and its public key, and nothing else");

    /// What a node says at startup about where its key came from, in enumerator order.
    constexpr auto OriginSentences = EnumTable<NodeKeyOrigin, std::string_view> {
        "recorded",
        "newly minted; no cluster has admitted this key yet",
    };

    /// What an operator is told to do about a file that is refused rather than replaced.
    ///
    /// One sentence for every refusal of a file that is THERE, because the remedy is the
    /// same and its cost has to be stated each time: removing the file is a new identity,
    /// and a cluster that admitted the old one will not know the new one.
    constexpr std::string_view RemoveDeliberately =
        "it is never replaced because it could not be used, since the key it holds may be the one this cluster "
        "admitted. Remove it deliberately to mint a new identity, which the cluster must then admit";

    /// Whether an exposure keeps a file from holding this node's identity key, one row per
    /// `SecretExposure`, so a new exposure is a decision somebody has to write down rather
    /// than one the array's zero-fill makes for them.
    struct KeyExposureRow
    {
        SecretExposure exposure; ///< What the guard found.
        bool refusedAtMint;      ///< Whether a key this node is about to write there is refused.
        bool refusedAtRead;      ///< Whether a key a previous start wrote there is refused.
    };

    /// See `RefusesKeyExposure`.
    constexpr auto KeyExposureTable = EnumTable<SecretExposure, KeyExposureRow> { {
        { .exposure = SecretExposure::None, .refusedAtMint = false, .refusedAtRead = false },
        { .exposure = SecretExposure::AnyLocalAccount, .refusedAtMint = true, .refusedAtRead = true },
        { .exposure = SecretExposure::OwnersOwnGroup, .refusedAtMint = true, .refusedAtRead = true },
        { .exposure = SecretExposure::Undetermined, .refusedAtMint = true, .refusedAtRead = false },
    } };
    static_assert(RowsInEnumeratorOrder(KeyExposureTable, &KeyExposureRow::exposure),
                  "one row per SecretExposure, in enumerator order");

    /// Whether a state directory's writers keep this node from trusting a key file in it.
    struct KeyDirectoryRow
    {
        DirectoryWriters writers; ///< What the guard found.
        bool refused;             ///< Whether the directory is refused.
    };

    /// See `RefusesKeyDirectory`. `Undetermined` is reported by nobody and refused by nobody,
    /// for `SecretExposure`'s reason at read: a filesystem that keeps no lists is ordinary.
    constexpr auto KeyDirectoryTable = EnumTable<DirectoryWriters, KeyDirectoryRow> { {
        { .writers = DirectoryWriters::OwnerOnly, .refused = false },
        { .writers = DirectoryWriters::Others, .refused = true },
        { .writers = DirectoryWriters::ForeignOwner, .refused = true },
        { .writers = DirectoryWriters::Undetermined, .refused = false },
    } };
    static_assert(RowsInEnumeratorOrder(KeyDirectoryTable, &KeyDirectoryRow::writers),
                  "one row per DirectoryWriters, in enumerator order");

    /// Flush a stream all the way to the disk, for `FileRaftStorage::FlushToDisk`'s reason:
    /// `fflush` alone reaches the kernel, which a power loss still discards -- and a key file
    /// a crash leaves EMPTY is one the next start refuses as truncated.
    /// @param file The open stream.
    /// @return True when both stages succeeded.
    [[nodiscard]] bool FlushToDisk(std::FILE* file) noexcept
    {
        if (std::fflush(file) != 0)
            return false;
#if defined(_WIN32)
        return ::_commit(::_fileno(file)) == 0;
#else
        return ::fsync(::fileno(file)) == 0;
#endif
    }

    /// A refusal, built in one place so the fault and the sentence travel together.
    /// @param fault What went wrong.
    /// @param message What an operator is told.
    /// @return The refusal, as an unexpected value.
    [[nodiscard]] std::unexpected<NodeKeyRefusal> Refuse(NodeKeyFault fault, std::string message)
    {
        return std::unexpected { NodeKeyRefusal { .fault = fault, .message = std::move(message) } };
    }

    /// Read the key file, if there is one.
    ///
    /// **Absent is what the OPEN says, and nothing else.** `ENOENT` -- the file, or the state
    /// directory, is not there -- is the one answer that mints. Every other way of not
    /// getting the bytes is a refusal, because a stat or an open that cannot answer, read as
    /// "absent", would mint over a key this machine holds.
    ///
    /// **And only a regular file of its own is read** (`OpenRegularFile`): never through a link,
    /// and never blocking -- a FIFO planted under the key's name would otherwise hold the start
    /// forever, before any judgement it was about to make. Anything else there is refused by name.
    ///
    /// One byte more than a key file is asked for, so a file that is too LONG is seen rather
    /// than read as its first 70 bytes.
    /// @param path The file.
    /// @return Its bytes, nothing when it is absent, or why it could not be read.
    [[nodiscard]] std::expected<std::optional<SecureByteBuffer>, NodeKeyRefusal> ReadKeyFile(
        std::filesystem::path const& path)
    {
        auto opened = OpenRegularFile(path);
        if (!opened.has_value())
        {
            auto const& refusal = opened.error();
            if (refusal.notRegular)
                return Refuse(NodeKeyFault::NotARegularFile,
                              std::format("{} is where this node keeps its identity key, and what is there is not a "
                                          "regular file -- a link, a FIFO, a device or a directory -- which this node "
                                          "never writes: somebody else put it there. Remove it; nothing was read and "
                                          "nothing was changed",
                                          path.string()));
            if (refusal.error == std::errc::no_such_file_or_directory)
                return std::optional<SecureByteBuffer> {};
            return Refuse(NodeKeyFault::Unreadable,
                          std::format("{} holds this node's identity key and cannot be opened: {}; {}{}",
                                      path.string(),
                                      refusal.error.message(),
                                      RemoveDeliberately,
                                      StateFileUnreadableHint(path)));
        }
        auto const file = *std::move(opened);

        SecureByteBuffer keyFileBytes(NodeKeyFileBytes + 1);
        auto const read = std::fread(keyFileBytes.data(), 1, keyFileBytes.size(), file.get());
        if (std::ferror(file.get()) != 0)
            return Refuse(
                NodeKeyFault::Unreadable,
                std::format("{} holds this node's identity key and cannot be read; {}", path.string(), RemoveDeliberately));
        keyFileBytes.resize(read);
        return std::optional { std::move(keyFileBytes) };
    }

    /// What an operator is told about a key others may have read, and what each answer costs.
    ///
    /// Two branches, because the remedy that works for a member of a larger cluster is one a
    /// cluster's ONLY voter cannot take: `--cluster-forget` refuses to remove the only voter,
    /// and there is no other voter to ask -- and a node with no cluster flags is exactly that.
    /// Both name what they touch: the id to forget, and the files that ARE this node's consensus.
    /// @param stateDirectory Where the node keeps its state.
    /// @param path The key file.
    /// @param exposure What was found.
    /// @return The sentence.
    [[nodiscard]] std::string ExposedKeyRefusal(std::filesystem::path const& stateDirectory,
                                                std::filesystem::path const& path,
                                                SecretExposure exposure)
    {
        auto state = std::format("{}", (stateDirectory / Cluster::FormationRecordFileName).string());
        for (auto const name: Consensus::FileRaftStorage::StoreFileNames())
            state += std::format(", {}", (stateDirectory / name).string());
        auto const id = RecordedNodeId(stateDirectory)
                            .value_or(std::format("<the id in {}>", (stateDirectory / NodeIdentityFileName).string()));
        return std::format(
            "{}. It holds this node's identity key, which is refused rather than used. If nothing can have read it, "
            "restrict it as that says and start again with the same identity. Otherwise treat the key as disclosed. "
            "If this node is its cluster's ONLY voter -- a node started with no cluster flags is one -- no other voter "
            "can forget it and --cluster-forget refuses to remove a cluster's only voter: remove {} and this node's "
            "consensus state ({}), and start it again. It mints a new key and a new one-voter cluster; hand its new "
            "public key to everything that trusted the old one. If it is a member of a larger cluster, run "
            "--cluster-forget={} against the leader, then remove {} to mint a new key, and admit that",
            OwnerOnlySecretExposureHint(path, exposure),
            path.string(),
            state,
            id,
            path.string());
    }

    /// Write a new key file, refusing when one appeared first.
    ///
    /// Created ALREADY protected and unshared (`CreateStateFile`, the key's row), the protection read
    /// back, and only then written: access is decided at open, so a list applied after the
    /// create would leave readable every handle opened in between -- and the secret would reach
    /// it through the later write.
    /// @param path The file.
    /// @param bytes Its contents.
    /// @param guard Who may read it, established before the bytes arrive.
    /// @return Nothing, or why it could not be written.
    [[nodiscard]] std::expected<void, NodeKeyRefusal> CreateKeyFile(std::filesystem::path const& path,
                                                                    std::span<std::byte const> bytes,
                                                                    INodeKeyFileGuard& guard)
    {
        auto created = CreateStateFile(path, StateFile::Key);
        if (!created.has_value())
        {
            // `EEXIST` keeps its own sentence: the read a moment ago found nothing, so a file
            // there now is another process minting into the same state directory -- and two
            // nodes sharing one directory is its own problem, not this key's.
            if (created.error() == std::errc::file_exists)
                return Refuse(NodeKeyFault::WriteFailed,
                              std::format("{} appeared while this node was minting its identity key: another process "
                                          "is using the state directory {}. Nothing was written",
                                          path.string(),
                                          path.parent_path().string()));
            return Refuse(
                NodeKeyFault::WriteFailed,
                std::format("cannot create {}: {}. Nothing was written", path.string(), created.error().message()));
        }
        auto file = *std::move(created);

        // Read back while it is still EMPTY. One whose protection does not read back is closed
        // before it is removed, because Windows will not delete a file this process holds open;
        // removing it discards nothing, and the next start mints afresh rather than refusing a
        // file that never held a key.
        if (RefusesKeyExposure(guard.Protect(path), KeyMoment::Mint))
        {
            file.reset();
            auto removeError = std::error_code {};
            auto const removed = std::filesystem::remove(path, removeError) && !removeError;
            return Refuse(NodeKeyFault::Unprotectable,
                          std::format("cannot make {} readable by its owner alone: after its permissions were "
                                      "restricted, other accounts could still read it, or who may read it could not be "
                                      "read back -- so no identity key was written into it and {}. Either the "
                                      "filesystem keeps no per-file permissions, or the directory's list keeps the "
                                      "file's owner from changing the file's own; name a state directory where neither "
                                      "holds with --cluster-dir",
                                      path.string(),
                                      removed ? "the empty file was removed"
                                              : "the empty file could not be removed; the next start refuses it as "
                                                "truncated until it is"));
        }

        // A partial write leaves a file the next start refuses as truncated, which is the
        // safe direction: a key that may have been seen is never silently replaced. The
        // sentence says so, because the file is left behind.
        if (std::fwrite(bytes.data(), 1, bytes.size(), file.get()) != bytes.size() || !FlushToDisk(file.get())
            || std::fclose(file.release()) != 0)
            return Refuse(NodeKeyFault::WriteFailed,
                          std::format("cannot write {} in full; the next start will refuse what was written, and "
                                      "removing it mints a new identity",
                                      path.string()));
        return {};
    }
} // namespace

SecretExposure FileTrustNodeKeyGuard::ExposureOf(std::filesystem::path const& file)
{
    return SecretFileExposure(file);
}

FileOwner FileTrustNodeKeyGuard::OwnerOf(std::filesystem::path const& file)
{
    return FileOwnerOf(file);
}

DirectoryWriters FileTrustNodeKeyGuard::WritersOf(std::filesystem::path const& directory)
{
    return DirectoryWritersOf(directory);
}

bool FileTrustNodeKeyGuard::IsLink(std::filesystem::path const& entry)
{
    return IsLinkEntry(entry);
}

std::expected<bool, std::error_code> FileTrustNodeKeyGuard::OthersMayWrite(std::filesystem::path const& entry)
{
    return FastCache::OthersMayWrite(entry);
}

SecretExposure FileTrustNodeKeyGuard::Protect(std::filesystem::path const& file)
{
    return SecureSecretFileForOwner(file);
}

bool RefusesKeyExposure(SecretExposure exposure, KeyMoment moment) noexcept
{
    auto const& row = KeyExposureTable[static_cast<std::size_t>(exposure)];
    return moment == KeyMoment::Mint ? row.refusedAtMint : row.refusedAtRead;
}

bool RefusesKeyDirectory(DirectoryWriters writers) noexcept
{
    return KeyDirectoryTable[static_cast<std::size_t>(writers)].refused;
}

std::string_view DescribeNodeKeyOrigin(NodeKeyOrigin origin) noexcept
{
    return OriginSentences[static_cast<std::size_t>(origin)];
}

std::filesystem::path NodeKeyPath(NodeConfig const& cfg)
{
    auto const chosen = ChosenStateDirectory(cfg);
    if (!chosen.has_value())
        return {};
    return chosen->path / NodeKeyFileName;
}

SecureByteBuffer EncodeNodeKeyFile(std::span<std::byte const> seed, Ed25519PublicKey const& publicKey)
{
    assert(seed.size() == Ed25519SeedBytes);
    auto const version = WireFields::ToBigEndian<std::uint16_t>(FormatVersion);

    SecureByteBuffer keyFileBytes;
    keyFileBytes.reserve(NodeKeyFileBytes);
    keyFileBytes.insert(keyFileBytes.end(), Magic.begin(), Magic.end());
    keyFileBytes.insert(keyFileBytes.end(), version.begin(), version.end());
    keyFileBytes.insert(keyFileBytes.end(), seed.begin(), seed.end());
    keyFileBytes.insert(keyFileBytes.end(), publicKey.begin(), publicKey.end());
    return keyFileBytes;
}

std::expected<Ed25519KeyPair, NodeKeyRefusal> DecodeNodeKeyFile(std::span<std::byte const> bytes,
                                                                std::filesystem::path const& path)
{
    // Too short to carry even a header: nearly always EMPTY, which is what a crash between
    // the create and the write leaves.
    if (bytes.size() < HeaderBytes)
        return Refuse(NodeKeyFault::Truncated,
                      std::format("{} is {} bytes long and a node key file is {}: a write that did not finish; {}",
                                  path.string(),
                                  bytes.size(),
                                  NodeKeyFileBytes,
                                  RemoveDeliberately));

    if (!std::ranges::equal(bytes.first(Magic.size()), Magic))
        return Refuse(NodeKeyFault::NotAKeyFile,
                      std::format("{} is not a node key file: it does not begin with one's header, so something other "
                                  "than this node wrote it; {}",
                                  path.string(),
                                  RemoveDeliberately));

    // The version BEFORE any length: see the declaration.
    auto const version = WireFields::FromBigEndian<std::uint16_t>(bytes.subspan(Magic.size(), sizeof(std::uint16_t)));
    if (version != FormatVersion)
        return Refuse(NodeKeyFault::ForeignFormat,
                      std::format("{} is a node key file in format {} and this build reads format {}. It is intact: "
                                  "run the build that wrote it; {}",
                                  path.string(),
                                  version.value_or(0),
                                  FormatVersion,
                                  RemoveDeliberately));

    if (bytes.size() < NodeKeyFileBytes)
        return Refuse(NodeKeyFault::Truncated,
                      std::format("{} is {} bytes long and a node key file is {}: a write that did not finish; {}",
                                  path.string(),
                                  bytes.size(),
                                  NodeKeyFileBytes,
                                  RemoveDeliberately));
    if (bytes.size() > NodeKeyFileBytes)
        return Refuse(NodeKeyFault::Damaged,
                      std::format("{} is longer than a node key file, so it is damaged or was written by something "
                                  "else; {}",
                                  path.string(),
                                  RemoveDeliberately));

    auto const seed = bytes.subspan(HeaderBytes, Ed25519SeedBytes);
    auto const stored = bytes.subspan(HeaderBytes + Ed25519SeedBytes, Ed25519PublicKeyBytes);
    auto pair = Ed25519KeyPair::FromSeed(seed);

    // The file's own integrity check: a flipped bit in the seed is another VALID key, and
    // only the stored public key can say this is not the one that was written.
    if (!pair.has_value() || !std::ranges::equal(pair->PublicKey(), stored))
        return Refuse(NodeKeyFault::Damaged,
                      std::format("{} is damaged: the public key it records is not the one its secret derives; {}",
                                  path.string(),
                                  RemoveDeliberately));
    return *std::move(pair);
}

std::expected<void, NodeKeyRefusal> JudgeStateDirectory(std::filesystem::path const& stateDirectory,
                                                        INodeKeyFileGuard& guard)
{
    auto missing = std::error_code {};
    if (!std::filesystem::is_directory(stateDirectory, missing))
        return {};

    // The WRITERS first: a directory other accounts may add to or delete from lets them plant
    // the key this node adopts, or remove it so this node mints a new identity and falls out of
    // its cluster with nothing but a `minted` line to say so.
    if (auto const writers = guard.WritersOf(stateDirectory); RefusesKeyDirectory(writers))
    {
        auto message = std::format("{}. It holds this node's identity key, so no key in it is trusted and none is "
                                   "minted there until it is; if another account may have put {} there, remove the "
                                   "file too",
                                   DirectoryWritersHint(stateDirectory, writers),
                                   NodeKeyFileName);

        // **The key's exposure is asked HERE, before the remedy is handed out.** Restricting the
        // directory propagates to a key that INHERITED its read, taking that read away -- and with
        // it the only evidence the key was exposed: the next start would find an owner-only key
        // and adopt it, the `Exposed` question never asked. So the question travels with the
        // directory's refusal, both branches of it.
        auto const key = stateDirectory / NodeKeyFileName;
        auto absent = std::error_code {};
        if (std::filesystem::is_regular_file(key, absent))
            if (auto const exposure = guard.ExposureOf(key); RefusesKeyExposure(exposure, KeyMoment::Read))
                message += std::format(". And answer this BEFORE running that: {} is readable by other accounts as "
                                       "well, and restricting the directory takes that read away from it, so nothing "
                                       "will show afterwards that it ever was. {}",
                                       key.string(),
                                       ExposedKeyRefusal(stateDirectory, key, exposure));
        return Refuse(NodeKeyFault::OpenDirectory, std::move(message));
    }

    // Then WHO put each file there. The key is one row of the state directory's table: a file
    // nobody else can READ may still be one somebody else WROTE -- here a secret they chose, and
    // beside it the id, the formation record and the consensus store the node acts on as surely.
    // Only this node's own account, or an administrative one, may have written any of them.
    return RefuseForeignStateFiles(stateDirectory, guard);
}

std::expected<NodeKey, NodeKeyRefusal> ResolveNodeKey(std::filesystem::path const& stateDirectory,
                                                      ISecureRandom& random,
                                                      INodeKeyFileGuard& guard)
{
    auto const path = stateDirectory / NodeKeyFileName;

    // WHO could have put a file there comes before the file is so much as opened
    // (`JudgeStateDirectory`): a directory somebody else may write in is refused before anything
    // they may have planted in it is touched. One this node creates below is created its owner's
    // alone.
    if (auto judged = JudgeStateDirectory(stateDirectory, guard); !judged.has_value())
        return std::unexpected { std::move(judged).error() };

    auto recorded = ReadKeyFile(path);
    if (!recorded.has_value())
        return std::unexpected { std::move(recorded).error() };
    auto missing = std::error_code {};
    auto const directoryExists = std::filesystem::is_directory(stateDirectory, missing);

    if (recorded->has_value())
    {
        auto pair = DecodeNodeKeyFile(**recorded, path);
        if (!pair.has_value())
            return std::unexpected { std::move(pair).error() };

        // After the decode, so a file that is not a usable key is refused for what is wrong with
        // it -- its remedy is removing it whoever may read it. Never tightened in place: that
        // would answer, silently, the one question only the operator can -- whether the key was
        // read while it was exposed.
        if (auto const exposure = guard.ExposureOf(path); RefusesKeyExposure(exposure, KeyMoment::Read))
            return Refuse(NodeKeyFault::Exposed, ExposedKeyRefusal(stateDirectory, path, exposure));
        return NodeKey { .pair = *std::move(pair), .origin = NodeKeyOrigin::Recorded };
    }

    // Drawn BEFORE anything is created, so a mint this host cannot draw leaves no directory
    // and no file behind -- and refused by name rather than filled from a weaker source,
    // #1527's rule for the node id arriving at the key that proves it.
    SecureByteBuffer identitySeed(Ed25519SeedBytes);
    if (auto const drawn = random.Fill(identitySeed); !drawn.has_value())
        return Refuse(NodeKeyFault::DrawFailed,
                      std::format("cannot mint an identity key into {}: {}. Nothing was written, and no weaker source "
                                  "is used in its place",
                                  path.string(),
                                  drawn.error().ToString()));

    auto pair = Ed25519KeyPair::FromSeed(identitySeed);
    if (!pair.has_value())
        return Refuse(NodeKeyFault::DrawFailed,
                      std::format("cannot mint an identity key into {}: {}", path.string(), CryptoErrorName(pair.error())));

    if (!directoryExists)
        if (auto const created = CreateOwnerOnlyDirectory(stateDirectory); !created.has_value())
            return Refuse(NodeKeyFault::WriteFailed,
                          std::format("cannot create {}: {}. Nothing was written",
                                      stateDirectory.string(),
                                      created.error().message()));

    if (auto created = CreateKeyFile(path, EncodeNodeKeyFile(identitySeed, pair->PublicKey()), guard); !created.has_value())
        return std::unexpected { std::move(created).error() };
    return NodeKey { .pair = *std::move(pair), .origin = NodeKeyOrigin::Minted };
}

std::expected<NodeKey, NodeKeyRefusal> ResolveNodeKeyFor(NodeConfig const& cfg,
                                                         ISecureRandom& random,
                                                         INodeKeyFileGuard& guard)
{
    return ResolveNodeKey(NodeStateDirectory(cfg), random, guard);
}

} // namespace FastCache::Node

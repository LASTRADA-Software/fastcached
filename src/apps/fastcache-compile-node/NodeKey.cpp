// SPDX-License-Identifier: Apache-2.0
#include "NodeIdentity.hpp"
#include "NodeKey.hpp"

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
    #include <sys/stat.h>

    #include <fcntl.h>
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

    /// The owning handle a `FILE*` is held in, so every failure path below closes it.
    using FileHandle = std::unique_ptr<std::FILE, int (*)(std::FILE*)>;

    /// Open @p path for reading, spelling it the way the platform wants.
    ///
    /// `_wfopen` on Windows, for `FileRaftStorage::OpenBinary`'s reason: a narrow path goes
    /// through the active code page, and a state directory holding a character that page
    /// cannot spell would fail to open for a reason that has nothing to do with the key.
    /// @param path The file.
    /// @param error Set to what the C library said when it could not.
    /// @return The stream, or null.
    [[nodiscard]] FileHandle OpenForReading(std::filesystem::path const& path, int& error)
    {
        errno = 0;
#if defined(_WIN32)
        gsl::owner<std::FILE*> const opened = ::_wfopen(path.wstring().c_str(), L"rb");
#else
        gsl::owner<std::FILE*> const opened = std::fopen(path.c_str(), "rb");
#endif
        error = errno;
        return FileHandle { opened, &std::fclose };
    }

    /// Create @p path for writing, refusing when it exists.
    ///
    /// The EXCLUSIVE create is the whole guard, with nothing in front of it
    /// (`StoreClusterKey`'s idiom): a file that appeared a moment ago is refused, never
    /// truncated. On POSIX the file is created mode 0600 by the same call, so there is no
    /// moment in which the secret is readable by another account and no `chmod` whose
    /// failure would leave it so.
    /// @param path The file.
    /// @param error Set to what the C library said when it could not.
    /// @return The stream, or null.
    [[nodiscard]] FileHandle CreateExclusively(std::filesystem::path const& path, int& error)
    {
        errno = 0;
#if defined(_WIN32)
        gsl::owner<std::FILE*> const opened = ::_wfopen(path.wstring().c_str(), L"wbx");
        error = errno;
        return FileHandle { opened, &std::fclose };
#else
        auto const descriptor =
            ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
        if (descriptor < 0)
        {
            error = errno;
            return FileHandle { nullptr, &std::fclose };
        }
        auto* const opened = ::fdopen(descriptor, "wb");
        if (opened == nullptr)
        {
            error = errno;
            ::close(descriptor);
        }
        return FileHandle { opened, &std::fclose };
#endif
    }

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

    /// The text of an `errno`, as the C library names it.
    /// @param error The value.
    /// @return Its message.
    [[nodiscard]] std::string ErrnoText(int error)
    {
        return std::error_code { error, std::generic_category() }.message();
    }

    /// Read the key file, if there is one.
    ///
    /// **Absent is what the OPEN says, and nothing else.** `ENOENT` -- the file, or the state
    /// directory, is not there -- is the one answer that mints. Every other way of not
    /// getting the bytes is a refusal, because a stat or an open that cannot answer, read as
    /// "absent", would mint over a key this machine holds.
    ///
    /// One byte more than a key file is asked for, so a file that is too LONG is seen rather
    /// than read as its first 70 bytes.
    /// @param path The file.
    /// @return Its bytes, nothing when it is absent, or why it could not be read.
    [[nodiscard]] std::expected<std::optional<SecureByteBuffer>, NodeKeyRefusal> ReadKeyFile(
        std::filesystem::path const& path)
    {
        auto openError = 0;
        auto const file = OpenForReading(path, openError);
        if (file == nullptr)
        {
            if (openError == ENOENT)
                return std::optional<SecureByteBuffer> {};
            return Refuse(NodeKeyFault::Unreadable,
                          std::format("{} holds this node's identity key and cannot be opened: {}; {}",
                                      path.string(),
                                      ErrnoText(openError),
                                      RemoveDeliberately));
        }

        SecureByteBuffer keyFileBytes(NodeKeyFileBytes + 1);
        auto const read = std::fread(keyFileBytes.data(), 1, keyFileBytes.size(), file.get());
        if (std::ferror(file.get()) != 0)
            return Refuse(
                NodeKeyFault::Unreadable,
                std::format("{} holds this node's identity key and cannot be read; {}", path.string(), RemoveDeliberately));
        keyFileBytes.resize(read);
        return std::optional { std::move(keyFileBytes) };
    }

    /// Write a new key file, refusing when one appeared first.
    /// @param path The file.
    /// @param bytes Its contents.
    /// @return Nothing, or why it could not be written.
    [[nodiscard]] std::expected<void, NodeKeyRefusal> CreateKeyFile(std::filesystem::path const& path,
                                                                    std::span<std::byte const> bytes)
    {
        auto createError = 0;
        auto file = CreateExclusively(path, createError);
        if (file == nullptr)
        {
            // `EEXIST` keeps its own sentence: the read a moment ago found nothing, so a file
            // there now is another process minting into the same state directory -- and two
            // nodes sharing one directory is its own problem, not this key's.
            if (createError == EEXIST)
                return Refuse(NodeKeyFault::WriteFailed,
                              std::format("{} appeared while this node was minting its identity key: another process "
                                          "is using the state directory {}. Nothing was written",
                                          path.string(),
                                          path.parent_path().string()));
            return Refuse(NodeKeyFault::WriteFailed,
                          std::format("cannot create {}: {}. Nothing was written", path.string(), ErrnoText(createError)));
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

std::string_view DescribeNodeKeyOrigin(NodeKeyOrigin origin) noexcept
{
    return OriginSentences[static_cast<std::size_t>(origin)];
}

bool HoldsNodeKey(NodeConfig const& cfg) noexcept
{
    return RunsConsensus(cfg) || !cfg.clusterDir.empty();
}

std::filesystem::path NodeKeyPath(NodeConfig const& cfg)
{
    if (!HoldsNodeKey(cfg))
        return {};
    return NodeStateDirectory(cfg) / NodeKeyFileName;
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

std::expected<NodeKey, NodeKeyRefusal> ResolveNodeKey(std::filesystem::path const& stateDirectory, ISecureRandom& random)
{
    auto const path = stateDirectory / NodeKeyFileName;

    auto recorded = ReadKeyFile(path);
    if (!recorded.has_value())
        return std::unexpected { std::move(recorded).error() };
    if (recorded->has_value())
    {
        auto pair = DecodeNodeKeyFile(**recorded, path);
        if (!pair.has_value())
            return std::unexpected { std::move(pair).error() };
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

    auto failure = std::error_code {};
    std::filesystem::create_directories(stateDirectory, failure);
    if (failure)
        return Refuse(NodeKeyFault::WriteFailed,
                      std::format("cannot create {}: {}. Nothing was written", stateDirectory.string(), failure.message()));

    if (auto created = CreateKeyFile(path, EncodeNodeKeyFile(identitySeed, pair->PublicKey())); !created.has_value())
        return std::unexpected { std::move(created).error() };
    return NodeKey { .pair = *std::move(pair), .origin = NodeKeyOrigin::Minted };
}

std::expected<std::optional<NodeKey>, NodeKeyRefusal> ResolveNodeKeyFor(NodeConfig const& cfg, ISecureRandom& random)
{
    if (!HoldsNodeKey(cfg))
        return std::optional<NodeKey> {};
    return ResolveNodeKey(NodeStateDirectory(cfg), random).transform([](NodeKey key) {
        return std::optional<NodeKey> { std::move(key) };
    });
}

} // namespace FastCache::Node

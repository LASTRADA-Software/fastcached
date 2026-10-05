// SPDX-License-Identifier: Apache-2.0
#include "NodeIdentity.hpp"
#include "NodeKey.hpp"

#include <FastCache/Cluster/FormationRecord.hpp>
#include <FastCache/Consensus/FileRaftStorage.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/ISecureRandom.hpp>
#include <FastCache/Platform/FileTrust.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <tests/AccessList.hpp>
#include <tests/BoundedWait.hpp>
#include <tests/HostNamingFakes.hpp>
#include <tests/NodeKeyFakes.hpp>
#include <tests/ScratchPath.hpp>
#include <tests/SecureRandomFakes.hpp>

#if !defined(_WIN32)
    #include <sys/stat.h>

    #include <fcntl.h>
    #include <unistd.h>
#endif

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Testing::ScratchDirectory;
using FastCache::Testing::ScriptedConfigPathProbe;
using FastCache::Testing::ScriptedNodeKeyGuard;
using FastCache::Testing::ScriptedSecureRandom;

namespace
{

/// The bytes of a file, or empty when it cannot be read.
/// @param path The file.
/// @return Its contents.
[[nodiscard]] std::vector<std::byte> BytesOf(std::filesystem::path const& path)
{
    // The sized read rather than `std::istreambuf_iterator`, for `ReadIdentityFile`'s reason:
    // GCC at `-O3` reports `-Werror=null-dereference` inside `<streambuf>` for the iterator.
    auto stream = std::ifstream { path, std::ios::binary | std::ios::ate };
    if (!stream.is_open())
        return {};
    auto const size = stream.tellg();
    if (size <= 0)
        return {};
    stream.seekg(0, std::ios::beg);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    stream.read(reinterpret_cast<char*>(bytes.data()), size);
    return bytes;
}

/// Write @p bytes to @p path, replacing whatever was there.
/// @param path The file.
/// @param bytes Its new contents.
void WriteBytes(std::filesystem::path const& path, std::span<std::byte const> bytes)
{
    auto stream = std::ofstream { path, std::ios::binary | std::ios::trunc };
    stream.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

/// Resolve, requiring success, so a case reads as the property it is about.
/// @param dir The state directory.
/// @param random Where a mint's seed comes from.
/// @param guard Who may read the key file.
/// @return The key.
[[nodiscard]] NodeKey Resolved(std::filesystem::path const& dir, ISecureRandom& random, INodeKeyFileGuard& guard)
{
    auto resolved = ResolveNodeKey(dir, random, guard);
    REQUIRE(resolved.has_value());
    return *std::move(resolved);
}

/// Resolve, requiring a REFUSAL, and return it.
/// @param dir The state directory.
/// @param random Where a mint's seed would come from.
/// @param guard Who may read the key file.
/// @return The refusal.
[[nodiscard]] NodeKeyRefusal Refused(std::filesystem::path const& dir, ISecureRandom& random, INodeKeyFileGuard& guard)
{
    // Copied out of a const local rather than moved, for a compiler rather than for taste:
    // `std::move(resolved).error()` made g++ 14.3 at -O3 -- the gate's gcc-release leg -- report
    // `-Wfree-nonheap-object` inside the expected's destructor, on the key's arm, which cannot
    // run here. This spelling builds clean there; the copy is one short string.
    auto const resolved = ResolveNodeKey(dir, random, guard);
    REQUIRE_FALSE(resolved.has_value());
    return resolved.error();
}

/// A key file as this build writes it, from a seed of ascending bytes.
/// @param first The seed's first byte.
/// @return The file's bytes.
[[nodiscard]] std::vector<std::byte> ValidKeyFile(std::uint8_t first)
{
    auto const seed = ScriptedSecureRandom::Ascending(Ed25519SeedBytes, first);
    auto const pair = Ed25519KeyPair::FromSeed(seed);
    REQUIRE(pair.has_value());
    auto const encoded = EncodeNodeKeyFile(seed, pair->PublicKey());
    return { encoded.begin(), encoded.end() };
}

/// A node that runs consensus, whose state directory is @p dir.
/// @param dir Where its state lives.
/// @return The configuration.
[[nodiscard]] NodeConfig ClusteredNode(std::filesystem::path const& dir)
{
    NodeConfig cfg;
    cfg.raftListen = "6680";
    cfg.raftSelf = "10.0.0.7";
    cfg.clusterDir = dir;
    return cfg;
}

} // namespace

TEST_CASE("A node mints its identity key once and a restart reads back the same key", "[node][identity][key]")
{
    // #178's acceptance, and the SECOND start is the assertion: a design that minted afresh
    // at every start would pass "a key came back" and fail this. A DIFFERENT source for the
    // restart, so a re-mint would produce a different key rather than the same one for the
    // wrong reason.
    ScratchDirectory const scratch { "node-key" };
    auto guard = ScriptedNodeKeyGuard::OwnerOnly();

    ScriptedSecureRandom first { ScriptedSecureRandom::Ascending(Ed25519SeedBytes, 0x10) };
    auto const minted = Resolved(scratch.Path(), first, guard);
    CHECK(minted.origin == NodeKeyOrigin::Minted);
    auto const onDisk = BytesOf(scratch.Path() / NodeKeyFileName);
    CHECK(onDisk.size() == NodeKeyFileBytes);

    ScriptedSecureRandom second { ScriptedSecureRandom::Ascending(Ed25519SeedBytes, 0x80) };
    auto const readBack = Resolved(scratch.Path(), second, guard);
    CHECK(readBack.origin == NodeKeyOrigin::Recorded);
    // The PUBLIC KEY, byte for byte -- the thing a roster records and a peer checks.
    CHECK(readBack.pair.PublicKey() == minted.pair.PublicKey());
    // READ rather than re-derived: the source was never consulted, and the file is untouched.
    CHECK(second.FillCount() == 0);
    CHECK(BytesOf(scratch.Path() / NodeKeyFileName) == onDisk);

    // And the key signs as the one it claims to be, so what was read back is a usable pair
    // rather than a public key that happens to match.
    auto const message = std::vector { std::byte { 0x2A } };
    CHECK(Ed25519Verify(minted.pair.PublicKey(), message, readBack.pair.Sign(message)));
}

TEST_CASE("A wiped state directory mints a new identity key", "[node][identity][key]")
{
    // The node-id rule's twin: the state directory IS the node, so losing it is a new node.
    // A key that survived the directory would be one derived from the machine, which is the
    // design #1024 rejected for the id and this rejects for the key.
    ScratchDirectory const scratch { "node-key-wiped" };
    auto guard = ScriptedNodeKeyGuard::OwnerOnly();
    auto const stateDir = scratch.Path() / "state";

    ScriptedSecureRandom first { ScriptedSecureRandom::Ascending(Ed25519SeedBytes, 0x01) };
    auto const before = Resolved(stateDir, first, guard).pair.PublicKey();

    auto removed = std::error_code {};
    std::filesystem::remove_all(stateDir, removed);
    REQUIRE_FALSE(removed);

    ScriptedSecureRandom second { ScriptedSecureRandom::Ascending(Ed25519SeedBytes, 0x40) };
    auto const after = Resolved(stateDir, second, guard);
    CHECK(after.origin == NodeKeyOrigin::Minted);
    CHECK(after.pair.PublicKey() != before);
    CHECK(second.FillCount() == 1);
}

TEST_CASE("A truncated key file is refused and never re-minted", "[node][identity][key]")
{
    // #178's acceptance. A write that did not finish leaves a short file, and a node that
    // "helpfully" minted over it would replace a key the cluster may have admitted -- becoming
    // a stranger with nothing anywhere saying why. The neuter this case exists for is exactly
    // that re-mint: it turns every check below red.
    ScratchDirectory const scratch { "node-key-truncated" };
    auto guard = ScriptedNodeKeyGuard::OwnerOnly();
    auto const path = scratch.Path() / NodeKeyFileName;
    auto const whole = ValidKeyFile(0x20);

    for (auto const length: { std::size_t { 0 }, std::size_t { 3 }, std::size_t { 40 }, NodeKeyFileBytes - 1 })
    {
        auto const truncated = std::span<std::byte const> { whole }.first(length);
        WriteBytes(path, truncated);

        ScriptedSecureRandom random { ScriptedSecureRandom::Ascending(Ed25519SeedBytes, 0x90) };
        auto const refusal = Refused(scratch.Path(), random, guard);
        CHECK(refusal.fault == NodeKeyFault::Truncated);
        CHECK(refusal.message.contains(path.string()));
        CHECK(refusal.message.contains("Remove it deliberately"));

        // Left EXACTLY as it was, and nothing drawn. A resolver that re-minted over the file
        // answers no refusal at all, and one that tried and was stopped by the exclusive create
        // answers a different fault having drawn a seed: both fail here.
        CHECK(BytesOf(path) == std::vector<std::byte> { truncated.begin(), truncated.end() });
        CHECK(random.FillCount() == 0);
    }
}

TEST_CASE("A key file that is not one, is another build's, or disagrees with itself is refused by what is wrong",
          "[node][identity][key]")
{
    // One fault per remedy: somebody else's file, an intact file another build laid out, and
    // a file whose two halves do not agree are three different things to go and do.
    ScratchDirectory const scratch { "node-key-malformed" };
    auto guard = ScriptedNodeKeyGuard::OwnerOnly();
    auto const path = scratch.Path() / NodeKeyFileName;

    auto const refusalFor = [&](std::vector<std::byte> const& bytes) {
        WriteBytes(path, bytes);
        ScriptedSecureRandom random { ScriptedSecureRandom::Ascending(Ed25519SeedBytes) };
        auto const refusal = Refused(scratch.Path(), random, guard);
        CHECK(random.FillCount() == 0);
        CHECK(BytesOf(path) == bytes);
        return refusal.fault;
    };

    auto wrongMagic = ValidKeyFile(0x30);
    wrongMagic[0] = std::byte { 'X' };
    CHECK(refusalFor(wrongMagic) == NodeKeyFault::NotAKeyFile);

    // Another build's layout: the version is judged BEFORE the length, so a shorter file in a
    // later format is still reported as intact-and-foreign rather than as truncated.
    auto foreign = ValidKeyFile(0x30);
    foreign[5] = std::byte { 2 };
    CHECK(refusalFor(foreign) == NodeKeyFault::ForeignFormat);
    foreign.resize(20);
    CHECK(refusalFor(foreign) == NodeKeyFault::ForeignFormat);

    // A flipped bit in the SEED is another valid key, which only the stored public key can
    // expose -- the reason the file carries both.
    auto flippedSeed = ValidKeyFile(0x30);
    flippedSeed[6] ^= std::byte { 0x01 };
    CHECK(refusalFor(flippedSeed) == NodeKeyFault::Damaged);

    auto overlong = ValidKeyFile(0x30);
    overlong.push_back(std::byte { 0 });
    CHECK(refusalFor(overlong) == NodeKeyFault::Damaged);
}

TEST_CASE("A key fault ends a one-shot command by its stage: an I/O arm transient, a verdict a decision",
          "[node][identity][key][exit]")
{
    // The OTHER reader of the same faults: `--print-identity` is read by an operator, and a retry may
    // get past an I/O arm (1) while a verdict on the bytes read is a decision (2). Written out here,
    // as the start's verdicts are, so a fault whose ending changes fails by name.
    struct FaultEnding
    {
        NodeKeyFault fault;
        CommandEnding ending;
    };
    constexpr auto verdicts = std::to_array<FaultEnding>({
        { .fault = NodeKeyFault::Unreadable, .ending = CommandEnding::Failed },
        { .fault = NodeKeyFault::Truncated, .ending = CommandEnding::Declined },
        { .fault = NodeKeyFault::NotAKeyFile, .ending = CommandEnding::Declined },
        { .fault = NodeKeyFault::ForeignFormat, .ending = CommandEnding::Declined },
        { .fault = NodeKeyFault::Damaged, .ending = CommandEnding::Declined },
        { .fault = NodeKeyFault::Exposed, .ending = CommandEnding::Declined },
        { .fault = NodeKeyFault::ForeignOwner, .ending = CommandEnding::Declined },
        { .fault = NodeKeyFault::OpenDirectory, .ending = CommandEnding::Declined },
        { .fault = NodeKeyFault::LinkEntry, .ending = CommandEnding::Declined },
        { .fault = NodeKeyFault::DrawFailed, .ending = CommandEnding::Failed },
        { .fault = NodeKeyFault::WriteFailed, .ending = CommandEnding::Failed },
        { .fault = NodeKeyFault::Unprotectable, .ending = CommandEnding::Failed },
        { .fault = NodeKeyFault::UnknownEntry, .ending = CommandEnding::Declined },
        { .fault = NodeKeyFault::OthersMayWrite, .ending = CommandEnding::Declined },
        { .fault = NodeKeyFault::NotARegularFile, .ending = CommandEnding::Declined },
        { .fault = NodeKeyFault::WritersUndetermined, .ending = CommandEnding::Failed },
    });
    for (auto const fault: Enumerators<NodeKeyFault>())
        CHECK(std::ranges::count(verdicts, fault, &FaultEnding::fault) == 1);
    for (auto const& verdict: verdicts)
    {
        INFO(static_cast<int>(verdict.fault));
        CHECK(EndingOf(verdict.fault) == verdict.ending);
    }
}

TEST_CASE("A key fault ends a start as a refusal only when it is a verdict on bytes that were read",
          "[node][identity][key][exit]")
{
    // Written out here rather than read back from `NodeKeyFaultStages`, so a fault whose stage is
    // changed -- or one added without a decision -- fails by name. An I/O arm is a FAILURE, whatever
    // its errno: the next start may open, write or draw.
    struct FaultVerdict
    {
        NodeKeyFault fault;
        StartStage stage;
    };
    constexpr auto verdicts = std::to_array<FaultVerdict>({
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
    });
    for (auto const fault: Enumerators<NodeKeyFault>())
        CHECK(std::ranges::count(verdicts, fault, &FaultVerdict::fault) == 1);
    for (auto const& verdict: verdicts)
    {
        INFO(static_cast<int>(verdict.fault));
        CHECK(StageOf(verdict.fault) == verdict.stage);
    }
    CHECK(ExitOf(StartStage::IdentityKey) == ProcessExit::Refused);
    CHECK(ExitOf(StartStage::IdentityKeyIo) == ProcessExit::Failed);
}

TEST_CASE("A key file that cannot be opened is refused, not treated as absent", "[node][identity][key]")
{
    // ABSENT is what the open says and nothing else; an open that fails any other way, read
    // as absent, would mint over a key this machine holds. A DIRECTORY where the file should
    // be is an entry that is there and is no key file on every platform, without needing a
    // permission this test cannot drop when it runs as root -- refused as what it is.
    ScratchDirectory const scratch { "node-key-unreadable" };
    auto guard = ScriptedNodeKeyGuard::OwnerOnly();
    std::filesystem::create_directories(scratch.Path() / NodeKeyFileName);

    ScriptedSecureRandom random { ScriptedSecureRandom::Ascending(Ed25519SeedBytes) };
    auto const refusal = Refused(scratch.Path(), random, guard);
    CHECK(refusal.fault == NodeKeyFault::NotARegularFile);
    // A verdict on what the filesystem says is there, which the next start reads the same way.
    CHECK(StageOf(refusal.fault) == StartStage::IdentityKey);
    CHECK(random.FillCount() == 0);
    CHECK(std::filesystem::is_directory(scratch.Path() / NodeKeyFileName));
}

TEST_CASE("A key the operating system cannot draw leaves nothing behind", "[node][identity][key]")
{
    // #1527's rule arriving at the key: refused by name, never filled from a weaker source, and
    // drawn BEFORE anything is created, so the directory is not even made.
    ScratchDirectory const scratch { "node-key-draw" };
    auto guard = ScriptedNodeKeyGuard::OwnerOnly();
    auto const stateDir = scratch.Path() / "state";

    ScriptedSecureRandom denied { ScriptedSecureRandom::DeniedFailure() };
    auto const refusal = Refused(stateDir, denied, guard);
    CHECK(refusal.fault == NodeKeyFault::DrawFailed);
    CHECK(refusal.message.contains("scripted-getrandom"));
    CHECK(denied.FillCount() == 1);
    CHECK_FALSE(std::filesystem::exists(stateDir));
}

TEST_CASE("A minted key file is protected while it is still empty, before the secret is written",
          "[node][identity][key][secret]")
{
    // The ORDER is the property: protected after the write, the secret would sit in a file whoever
    // the directory lets read could open, for as long as the write took -- and for good if the
    // protection then failed. So the size AT the protect is the assertion, not only that it ran.
    ScratchDirectory const scratch { "node-key-protect" };
    auto guard = ScriptedNodeKeyGuard::OwnerOnly();
    ScriptedSecureRandom random { ScriptedSecureRandom::Ascending(Ed25519SeedBytes) };

    auto const minted = Resolved(scratch.Path(), random, guard);
    CHECK(minted.origin == NodeKeyOrigin::Minted);
    REQUIRE(guard.Protected().size() == 1);
    CHECK(guard.Protected().front().file == scratch.Path() / NodeKeyFileName);
    CHECK(guard.Protected().front().sizeWhenProtected == 0);
    CHECK(BytesOf(scratch.Path() / NodeKeyFileName).size() == NodeKeyFileBytes);

    // A mint asks nothing about exposure: the file it just created has no history to judge.
    CHECK(guard.Asked().empty());
}

TEST_CASE("A mint whose key file cannot be made its owner's alone writes no key and leaves no file",
          "[node][identity][key][secret]")
{
    // Refused by name, with the empty file removed, so the next start mints afresh -- a file left
    // behind would be refused as truncated at every start after, for a key that never existed.
    // `Undetermined` among them: at MINT a protection that cannot be read back is no protection
    // this node can claim, and writing the secret anyway would make the order property vacuous.
    for (auto const afterProtect:
         { SecretExposure::AnyLocalAccount, SecretExposure::OwnersOwnGroup, SecretExposure::Undetermined })
    {
        CAPTURE(afterProtect);
        ScratchDirectory const scratch { "node-key-unprotectable" };
        ScriptedNodeKeyGuard guard { { .found = SecretExposure::None,
                                       .afterProtect = afterProtect,
                                       .owner = FileOwnerStanding::ThisProcess,
                                       .writers = DirectoryWriters::OwnerOnly } };
        ScriptedSecureRandom random { ScriptedSecureRandom::Ascending(Ed25519SeedBytes) };

        auto const refusal = Refused(scratch.Path(), random, guard);
        CHECK(refusal.fault == NodeKeyFault::Unprotectable);
        CHECK(refusal.message.contains((scratch.Path() / NodeKeyFileName).string()));
        CHECK(refusal.message.contains("the empty file was removed"));
        // Both causes, since the remedy differs: the filesystem, or the directory's list.
        CHECK(refusal.message.contains("per-file permissions"));
        CHECK(refusal.message.contains("the directory's list"));
        CHECK(guard.Protected().size() == 1);
        CHECK_FALSE(std::filesystem::exists(scratch.Path() / NodeKeyFileName));
    }
}

TEST_CASE("A mint on a volume that cannot sync a directory keeps its key and any other failed sync refuses it",
          "[node][identity][key]")
{
    // B4-7: the mint judges its directory sync by the table every state write uses. Refused on such
    // a volume, the FIRST start failed while the second read the key it had written and ran degraded.
    // Every row of this platform's table, so a row the mint does not honour cannot hide behind another.
    for (auto const& code: Consensus::UnsupportedDirectorySyncAnswers())
    {
        CAPTURE(code.value());
        ScratchDirectory const scratch { "node-key-unsyncable" };
        auto guard = ScriptedNodeKeyGuard::OwnerOnly();
        guard.DirectorySyncFails({ .step = Consensus::DirectorySyncStep::Flush, .code = code });
        ScriptedSecureRandom random { ScriptedSecureRandom::Ascending(Ed25519SeedBytes, 0x20) };

        auto const minted = Resolved(scratch.Path(), random, guard);
        CHECK(minted.origin == NodeKeyOrigin::Minted);
        CHECK(guard.Synced() == std::vector { scratch.Path() });
        CHECK(BytesOf(scratch.Path() / NodeKeyFileName).size() == NodeKeyFileBytes);
    }

    // The control, both ways: a sync that FAILED rather than one the volume does not offer -- an I/O
    // error from the flush, and a table code from the OPEN, which is a bad request rather than a
    // volume property -- still refuses, so the table did not turn into "ignore the sync".
    for (auto const failure:
         { Consensus::DirectorySyncFailure { .step = Consensus::DirectorySyncStep::Flush,
                                             .code = std::make_error_code(std::errc::io_error) },
           Consensus::DirectorySyncFailure { .step = Consensus::DirectorySyncStep::Open,
                                             .code = Consensus::UnsupportedDirectorySyncAnswers().front() } })
    {
        CAPTURE(failure.code.value());
        ScratchDirectory const scratch { "node-key-sync-failed" };
        auto guard = ScriptedNodeKeyGuard::OwnerOnly();
        guard.DirectorySyncFails(failure);
        ScriptedSecureRandom random { ScriptedSecureRandom::Ascending(Ed25519SeedBytes, 0x20) };

        auto const refusal = Refused(scratch.Path(), random, guard);
        CHECK(refusal.fault == NodeKeyFault::WriteFailed);
        CHECK(refusal.message.contains("cannot sync its directory"));
    }
}

TEST_CASE("One exposure rule judges a key at both ends and differs in one cell", "[node][identity][key][secret]")
{
    // Walked over the enumeration, so a new exposure is judged here with no edit. Refused at read
    // is refused at mint; the one cell that differs is `Undetermined`, which a node that ran
    // yesterday must keep reading and a node about to write a secret must not write into.
    for (auto const exposure: Enumerators<SecretExposure>())
    {
        CAPTURE(exposure);
        if (RefusesKeyExposure(exposure, KeyMoment::Read))
            CHECK(RefusesKeyExposure(exposure, KeyMoment::Mint));
    }
    CHECK(RefusesKeyExposure(SecretExposure::Undetermined, KeyMoment::Mint));
    CHECK_FALSE(RefusesKeyExposure(SecretExposure::Undetermined, KeyMoment::Read));
    CHECK_FALSE(RefusesKeyExposure(SecretExposure::None, KeyMoment::Mint));
}

TEST_CASE("A recorded key file other accounts may read is refused by name and left untouched",
          "[node][identity][key][secret]")
{
    // Never tightened in place and never re-minted -- whether the key was read while it was
    // exposed is the operator's question, and a node that tightened it silently would have
    // answered it for them. `Undetermined` is read back: a node that ran yesterday keeps running.
    struct Case
    {
        SecretExposure found;
        bool refused;
    };
    for (auto const& [found, refused]: { Case { .found = SecretExposure::None, .refused = false },
                                         Case { .found = SecretExposure::AnyLocalAccount, .refused = true },
                                         Case { .found = SecretExposure::OwnersOwnGroup, .refused = true },
                                         Case { .found = SecretExposure::Undetermined, .refused = false } })
    {
        CAPTURE(found);
        CHECK(RefusesKeyExposure(found, KeyMoment::Read) == refused);

        ScratchDirectory const scratch { "node-key-exposed" };
        auto const path = scratch.Path() / NodeKeyFileName;
        auto const bytes = ValidKeyFile(0x44);
        WriteBytes(path, bytes);
        ScriptedNodeKeyGuard guard { { .found = found,
                                       .afterProtect = SecretExposure::None,
                                       .owner = FileOwnerStanding::ThisProcess,
                                       .writers = DirectoryWriters::OwnerOnly } };
        ScriptedSecureRandom random { ScriptedSecureRandom::Ascending(Ed25519SeedBytes) };

        auto const resolved = ResolveNodeKey(scratch.Path(), random, guard);
        CHECK(resolved.has_value() == !refused);
        if (!resolved.has_value())
        {
            CHECK(resolved.error().fault == NodeKeyFault::Exposed);
            CHECK(resolved.error().message.contains(path.string()));
            CHECK(resolved.error().message.contains("restrict it"));
        }
        // Asked of the key file, and nothing written, drawn or protected either way.
        CHECK(guard.Asked() == std::vector { path });
        CHECK(guard.Protected().empty());
        CHECK(random.FillCount() == 0);
        CHECK(BytesOf(path) == bytes);
    }
}

TEST_CASE("The disclosed-key remedy branches on whether this node is its cluster's only voter",
          "[node][identity][key][secret]")
{
    // A node with no cluster flags is a one-voter cluster, and `--cluster-forget` refuses to remove
    // a cluster's only voter -- so "forget it" is advice that node cannot take. The sentence says
    // which files ARE its consensus, and the id a larger cluster forgets it by.
    ScratchDirectory const scratch { "node-key-remedy" };
    auto const path = scratch.Path() / NodeKeyFileName;
    WriteBytes(path, ValidKeyFile(0x45));
    auto guard = ScriptedNodeKeyGuard { { .found = SecretExposure::AnyLocalAccount,
                                          .afterProtect = SecretExposure::None,
                                          .owner = FileOwnerStanding::ThisProcess,
                                          .writers = DirectoryWriters::OwnerOnly } };
    ScriptedSecureRandom random { ScriptedSecureRandom::Ascending(Ed25519SeedBytes) };

    SECTION("the only voter removes the key AND its consensus state")
    {
        auto const refusal = Refused(scratch.Path(), random, guard);
        REQUIRE(refusal.fault == NodeKeyFault::Exposed);
        CHECK(refusal.message.contains("ONLY voter"));
        CHECK(refusal.message.contains("refuses to remove a cluster's only voter"));
        for (auto const name: Consensus::FileRaftStorage::StoreFileNames())
            CHECK(refusal.message.contains((scratch.Path() / name).string()));
        CHECK(refusal.message.contains((scratch.Path() / Cluster::FormationRecordFileName).string()));
        CHECK(refusal.message.contains("new one-voter cluster"));
        // With no id recorded, it names the file an id would be in rather than inventing one.
        CHECK(refusal.message.contains(std::format("<the id in {}>", (scratch.Path() / NodeIdentityFileName).string())));
    }
    SECTION("a member of a larger cluster is forgotten by the id it is recorded under")
    {
        WriteBytes(scratch.Path() / NodeIdentityFileName, std::as_bytes(std::span { std::string_view { "n-7f3a\n" } }));
        auto const refusal = Refused(scratch.Path(), random, guard);
        REQUIRE(refusal.fault == NodeKeyFault::Exposed);
        CHECK(refusal.message.contains("--cluster-forget=n-7f3a against the leader"));
    }
}

TEST_CASE("A key file another account owns is refused by name, however private it is", "[node][identity][key][secret]")
{
    // Nobody else can READ it, and somebody else WROTE it: a key they chose, which this node would
    // prove itself with. This node's own account and an administrative one may have minted it.
    struct Case
    {
        FileOwnerStanding owner;
        bool refused;
    };
    for (auto const& [owner, refused]: { Case { .owner = FileOwnerStanding::ThisProcess, .refused = false },
                                         Case { .owner = FileOwnerStanding::Administrative, .refused = false },
                                         Case { .owner = FileOwnerStanding::Another, .refused = true },
                                         Case { .owner = FileOwnerStanding::Undetermined, .refused = false } })
    {
        CAPTURE(owner);
        ScratchDirectory const scratch { "node-key-foreign" };
        auto const path = scratch.Path() / NodeKeyFileName;
        auto const bytes = ValidKeyFile(0x46);
        WriteBytes(path, bytes);
        ScriptedNodeKeyGuard guard { { .found = SecretExposure::None,
                                       .afterProtect = SecretExposure::None,
                                       .owner = owner,
                                       .writers = DirectoryWriters::OwnerOnly } };
        ScriptedSecureRandom random { ScriptedSecureRandom::Ascending(Ed25519SeedBytes) };

        auto const resolved = ResolveNodeKey(scratch.Path(), random, guard);
        CHECK(resolved.has_value() == !refused);
        if (!resolved.has_value())
        {
            CHECK(resolved.error().fault == NodeKeyFault::ForeignOwner);
            CHECK(resolved.error().message.contains("scripted-owner"));
            CHECK(resolved.error().message.contains(path.string()));
        }
        CHECK(random.FillCount() == 0);
        CHECK(BytesOf(path) == bytes);
    }
}

TEST_CASE("A state directory other accounts can write in holds no key this node trusts or mints",
          "[node][identity][key][secret]")
{
    // Whoever may add to or delete from the directory decides which key this node finds there, and
    // whether it finds one at all -- a deleted key is a silent re-mint. Refused whether a key is
    // recorded or not, with nothing drawn and nothing written.
    for (auto const writers: Enumerators<DirectoryWriters>())
    {
        CAPTURE(writers);
        auto const refused = writers == DirectoryWriters::Others || writers == DirectoryWriters::ForeignOwner;
        CHECK(RefusesKeyDirectory(writers) == refused);

        for (auto const recorded: { false, true })
        {
            CAPTURE(recorded);
            ScratchDirectory const scratch { "node-key-open-directory" };
            auto const path = scratch.Path() / NodeKeyFileName;
            auto const bytes = ValidKeyFile(0x47);
            if (recorded)
                WriteBytes(path, bytes);
            ScriptedNodeKeyGuard guard { { .found = SecretExposure::None,
                                           .afterProtect = SecretExposure::None,
                                           .owner = FileOwnerStanding::ThisProcess,
                                           .writers = writers } };
            ScriptedSecureRandom random { ScriptedSecureRandom::Ascending(Ed25519SeedBytes) };

            auto const resolved = ResolveNodeKey(scratch.Path(), random, guard);
            CHECK(resolved.has_value() == !refused);
            CHECK(guard.Directories() == std::vector { scratch.Path() });
            if (!resolved.has_value())
            {
                CHECK(resolved.error().fault == NodeKeyFault::OpenDirectory);
                CHECK(resolved.error().message.contains(scratch.Path().string()));
                CHECK(random.FillCount() == 0);
                CHECK(guard.Protected().empty());
                CHECK(BytesOf(path) == (recorded ? bytes : std::vector<std::byte> {}));
            }
        }
    }
}

TEST_CASE("A state directory this node creates is its owner's alone from the start", "[node][identity][key][secret]")
{
    // Not asked of a directory that is not there -- the node makes it, and makes it private. On
    // Windows a protected list of SYSTEM, Administrators and OWNER RIGHTS inherited by what is
    // made in it; on POSIX 0700. Whatever `%ProgramData%` lets every account do stops at it.
    ScratchDirectory const scratch { "node-key-new-directory" };
    auto const stateDir = scratch.Path() / "fresh" / "state";

    SECTION("the resolver asks nothing of a directory it is about to create")
    {
        auto guard = ScriptedNodeKeyGuard::OwnerOnly();
        ScriptedSecureRandom random { ScriptedSecureRandom::Ascending(Ed25519SeedBytes) };
        CHECK(Resolved(stateDir, random, guard).origin == NodeKeyOrigin::Minted);
        CHECK(guard.Directories().empty());
    }
    SECTION("and the directory it created reads back as its owner's alone")
    {
        FileTrustNodeKeyGuard guard;
        ScriptedSecureRandom random { ScriptedSecureRandom::Ascending(Ed25519SeedBytes) };
        CHECK(Resolved(stateDir, random, guard).origin == NodeKeyOrigin::Minted);
        CHECK(DirectoryWritersOf(stateDir) == DirectoryWriters::OwnerOnly);
#if defined(_WIN32)
        CHECK(Testing::AccessListOf(stateDir) == "D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;OW)");
#else
        using std::filesystem::perms;
        CHECK((std::filesystem::status(stateDir).permissions() & perms::all) == perms::owner_all);
#endif
    }
}

TEST_CASE("A real state directory other accounts can write in is refused with the command that restricts it",
          "[node][identity][key][secret]")
{
    // The production guard over the real filesystem: `BUILTIN\Users` add-file and add-subdirectory,
    // measured on this project's own `%ProgramData%\fastcache-node`, and a group- and world-writable
    // POSIX directory without the sticky bit.
    ScratchDirectory const scratch { "node-key-writable-directory" };
    auto const stateDir = scratch.Path() / "state";
    std::filesystem::create_directories(stateDir);
#if defined(_WIN32)
    REQUIRE(Testing::ApplyAccessList(stateDir, L"D:P(A;OICI;FA;;;OW)(A;OICI;FA;;;SY)(A;OICI;0x1200af;;;BU)"));
#else
    using std::filesystem::perms;
    std::filesystem::permissions(stateDir, perms::all);
#endif

    FileTrustNodeKeyGuard guard;
    ScriptedSecureRandom random { ScriptedSecureRandom::Ascending(Ed25519SeedBytes) };
    auto const refusal = Refused(stateDir, random, guard);
    CHECK(refusal.fault == NodeKeyFault::OpenDirectory);
    // Every command of the remedy, as `DirectoryWritersRemedy` spells it.
    auto const remedy = DirectoryWritersRemedy(stateDir, DirectoryWriters::Others);
    REQUIRE_FALSE(remedy.empty());
    for (auto const& command: remedy)
        CHECK(refusal.message.contains(command));
    CHECK_FALSE(std::filesystem::exists(stateDir / NodeKeyFileName));

#if !defined(_WIN32)
    // The sticky bit is NOT the answer for a state directory: it stops others removing an entry,
    // not creating a name that is not there yet -- this very key, before its first mint.
    std::filesystem::permissions(stateDir, perms::all | perms::sticky_bit);
    CHECK(Refused(stateDir, random, guard).fault == NodeKeyFault::OpenDirectory);
    CHECK_FALSE(std::filesystem::exists(stateDir / NodeKeyFileName));
#endif
}

TEST_CASE("A minted key file is its owner's alone, whatever its directory lets others read", "[node][identity][key][secret]")
{
    // The production guard over the real filesystem. The state directory is made broadly readable
    // FIRST -- the list a directory under `%ProgramData%` carries, inherited by what is created in
    // it -- so a key file that took its directory's list would fail here rather than pass because
    // the scratch directory happened to be private.
    ScratchDirectory const scratch { "node-key-owner-only" };
    auto const stateDir = scratch.Path() / "state";
    std::filesystem::create_directories(stateDir);
#if defined(_WIN32)
    REQUIRE(Testing::ApplyAccessList(stateDir, L"D:P(A;OICI;FA;;;OW)(A;OICI;FA;;;SY)(A;OICI;FR;;;BU)"));
#else
    using std::filesystem::perms;
    std::filesystem::permissions(
        stateDir, perms::owner_all | perms::group_read | perms::group_exec | perms::others_read | perms::others_exec);
#endif

    FileTrustNodeKeyGuard guard;
    ScriptedSecureRandom random { ScriptedSecureRandom::Ascending(Ed25519SeedBytes) };
    REQUIRE(Resolved(stateDir, random, guard).origin == NodeKeyOrigin::Minted);

    auto const path = stateDir / NodeKeyFileName;
    CHECK(SecretFileExposure(path) == SecretExposure::None);
    CHECK(FileOwnerOf(path).standing == FileOwnerStanding::ThisProcess);
#if defined(_WIN32)
    CHECK(Testing::AccessListOf(path) == "D:PAI(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;OW)");
#else
    auto const mode = std::filesystem::status(path).permissions();
    CHECK((mode & (perms::group_all | perms::others_all)) == perms::none);
    CHECK((mode & perms::owner_read) == perms::owner_read);
#endif

    // And its owner reads it back: SECURED, not BROKEN.
    ScriptedSecureRandom unused { ScriptedSecureRandom::Ascending(Ed25519SeedBytes, 0x80) };
    CHECK(Resolved(stateDir, unused, guard).origin == NodeKeyOrigin::Recorded);
    CHECK(unused.FillCount() == 0);
}

#if defined(_WIN32)
namespace
{
/// The production guard, with an attempt to open the key file made at the one moment between its
/// create and its write: what the protection reads back is the production answer, and the open is
/// what a local account racing the mint would try.
class RacingReaderGuard final: public INodeKeyFileGuard
{
  public:
    [[nodiscard]] SecretExposure ExposureOf(std::filesystem::path const& file) override
    {
        return _real.ExposureOf(file);
    }
    [[nodiscard]] FileOwner OwnerOf(std::filesystem::path const& file) override
    {
        return _real.OwnerOf(file);
    }
    [[nodiscard]] DirectoryWriters WritersOf(std::filesystem::path const& directory) override
    {
        return _real.WritersOf(directory);
    }

    [[nodiscard]] bool IsLink(std::filesystem::path const& entry) override
    {
        return _real.IsLink(entry);
    }

    [[nodiscard]] std::expected<bool, std::error_code> OthersMayWrite(std::filesystem::path const& entry) override
    {
        return _real.OthersMayWrite(entry);
    }
    [[nodiscard]] std::expected<void, Consensus::DirectorySyncFailure> SyncDirectory(
        std::filesystem::path const& directory) override
    {
        return _real.SyncDirectory(directory);
    }
    [[nodiscard]] SecretExposure Protect(std::filesystem::path const& file) override
    {
        {
            std::ifstream const racer { file, std::ios::binary };
            _racerOpened = racer.is_open();
        }
        _listAtCreate = Testing::AccessListOf(file);
        return _real.Protect(file);
    }

    /// @return Whether the racing open succeeded.
    [[nodiscard]] bool RacerOpened() const noexcept
    {
        return _racerOpened;
    }

    /// @return The file's list before anything but the create touched it.
    [[nodiscard]] std::string const& ListAtCreate() const noexcept
    {
        return _listAtCreate;
    }

  private:
    FileTrustNodeKeyGuard _real;
    bool _racerOpened { true };
    std::string _listAtCreate;
};
} // namespace

TEST_CASE("No handle to a key file can be opened between its create and its write", "[node][identity][key][secret]")
{
    // Access is decided at OPEN: a list applied after the create leaves readable every handle
    // opened in between, and the secret written later reaches it. So the file is created with its
    // list already in place and with share mode 0 -- the racing open fails, and the list it would
    // have been judged by is the owner-only one from the first instant, never the directory's.
    ScratchDirectory const scratch { "node-key-racer" };
    auto const stateDir = scratch.Path() / "state";
    std::filesystem::create_directories(stateDir);
    REQUIRE(Testing::ApplyAccessList(stateDir, L"D:P(A;OICI;FA;;;OW)(A;OICI;FA;;;SY)(A;OICI;FR;;;BU)"));

    RacingReaderGuard guard;
    ScriptedSecureRandom random { ScriptedSecureRandom::Ascending(Ed25519SeedBytes) };
    REQUIRE(Resolved(stateDir, random, guard).origin == NodeKeyOrigin::Minted);
    CHECK_FALSE(guard.RacerOpened());
    CHECK(guard.ListAtCreate().starts_with("D:P"));
    CHECK(guard.ListAtCreate().contains("(A;;FA;;;OW)"));
    CHECK_FALSE(guard.ListAtCreate().contains("BU"));
}
#endif

TEST_CASE("Every node holds an identity key in its state directory", "[node][identity][key]")
{
    // #178: every Raft peer connection proves each end's own key, and lane 2b's admission needs a
    // proven key on EVERY node -- a worker included. Every node has a state directory now: the one
    // it names, or the default resolved for it (here through the scripted probe, into scratch).
    ScratchDirectory const scratch { "node-key-every" };
    auto const probe = ScriptedConfigPathProbe { { { "LOCALAPPDATA", scratch.Path().string() },
                                                   { "XDG_STATE_HOME", scratch.Path().string() } },
                                                 ScriptedConfigPathProbe::Privilege::Unprivileged };

    auto named = ClusteredNode(scratch.Path());
    auto defaulted = named;
    defaulted.clusterDir.clear();
    ApplyNodeStateDirectory(defaulted, probe);
    auto worker = NodeConfig {};
    worker.raftListen.clear();
    ApplyNodeStateDirectory(worker, probe);

    for (auto const* const cfg: { &named, &defaulted, &worker })
        CHECK_FALSE(NodeKeyPath(*cfg).empty());
    CHECK(NodeKeyPath(defaulted) == scratch.Path() / NodeStateDirectoryName / NodeKeyFileName);
    CHECK(NodeKeyPath(worker) == NodeKeyPath(defaulted));
}

TEST_CASE("A node's key lives in the directory it names, or the default it resolved -- never before that",
          "[node][identity][key]")
{
    ScratchDirectory const scratch { "node-key-where" };
    auto guard = ScriptedNodeKeyGuard::OwnerOnly();

    auto const clustered = ClusteredNode(scratch.Path());
    CHECK(NodeKeyPath(clustered) == scratch.Path() / NodeKeyFileName);

    NodeConfig worker;
    worker.clusterDir = scratch.Path() / "worker";
    CHECK(NodeKeyPath(worker) == scratch.Path() / "worker" / NodeKeyFileName);

    // Before the default is resolved there is no path, and resolving the key is the precondition
    // it is -- it would otherwise mint in the working directory, which is `System32` under a
    // service. Nothing is drawn.
    NodeConfig unresolved;
    CHECK(NodeKeyPath(unresolved).empty());
    ScriptedSecureRandom random { ScriptedSecureRandom::Ascending(Ed25519SeedBytes) };
    CHECK_THROWS_AS(ResolveNodeKeyFor(unresolved, random, guard), std::logic_error);
    CHECK(random.FillCount() == 0);

    // And resolved, the same call mints where `NodeKeyPath` says.
    auto defaulted = NodeConfig {};
    ApplyNodeStateDirectory(defaulted,
                            ScriptedConfigPathProbe { { { "LOCALAPPDATA", scratch.Path().string() },
                                                        { "XDG_STATE_HOME", scratch.Path().string() } },
                                                      ScriptedConfigPathProbe::Privilege::Unprivileged });
    auto const held = ResolveNodeKeyFor(defaulted, random, guard);
    REQUIRE(held.has_value());
    CHECK(std::filesystem::exists(NodeKeyPath(defaulted)));
    CHECK(NodeKeyPath(defaulted).parent_path() == scratch.Path() / NodeStateDirectoryName);

    auto const named = ResolveNodeKeyFor(worker, random, guard);
    REQUIRE(named.has_value());
    CHECK(std::filesystem::exists(NodeKeyPath(worker)));
}

TEST_CASE("A key file round-trips through its format, and every origin has a sentence", "[node][identity][key]")
{
    auto const bytes = ValidKeyFile(0x55);
    REQUIRE(bytes.size() == NodeKeyFileBytes);
    auto const pair = DecodeNodeKeyFile(bytes, "node-key");
    REQUIRE(pair.has_value());
    auto const seed = ScriptedSecureRandom::Ascending(Ed25519SeedBytes, 0x55);
    auto const expected = Ed25519KeyPair::FromSeed(seed);
    REQUIRE(expected.has_value());
    CHECK(pair->PublicKey() == expected->PublicKey());

    CHECK_FALSE(DescribeNodeKeyOrigin(NodeKeyOrigin::Recorded).empty());
    CHECK(DescribeNodeKeyOrigin(NodeKeyOrigin::Minted).contains("newly minted"));
}

TEST_CASE("A state directory's refusal asks the key's exposure question when the key is readable by others",
          "[node][identity][key][secret]")
{
    // Restricting the directory propagates to a key that inherited its read, so after the remedy
    // nothing shows the key was ever exposed. The question travels with the directory's refusal,
    // and only when there is something to ask.
    struct Case
    {
        SecretExposure found;
        bool asked;
    };
    for (auto const& [found, asked]: { Case { .found = SecretExposure::AnyLocalAccount, .asked = true },
                                       Case { .found = SecretExposure::None, .asked = false } })
    {
        CAPTURE(found);
        ScratchDirectory const scratch { "node-key-open-exposed" };
        auto const path = scratch.Path() / NodeKeyFileName;
        auto const bytes = ValidKeyFile(0x52);
        WriteBytes(path, bytes);
        ScriptedNodeKeyGuard guard { { .found = found,
                                       .afterProtect = SecretExposure::None,
                                       .owner = FileOwnerStanding::ThisProcess,
                                       .writers = DirectoryWriters::Others } };
        ScriptedSecureRandom random { ScriptedSecureRandom::Ascending(Ed25519SeedBytes) };

        auto const resolved = ResolveNodeKey(scratch.Path(), random, guard);
        REQUIRE_FALSE(resolved.has_value());
        CHECK(resolved.error().fault == NodeKeyFault::OpenDirectory);
        CHECK(resolved.error().message.contains("answer this BEFORE running that") == asked);
        CHECK(resolved.error().message.contains("ONLY voter") == asked);
        CHECK(BytesOf(path) == bytes);
    }

    // With no key there is nothing to ask.
    ScratchDirectory const empty { "node-key-open-empty" };
    auto guard = ScriptedNodeKeyGuard { { .found = SecretExposure::AnyLocalAccount,
                                          .afterProtect = SecretExposure::None,
                                          .owner = FileOwnerStanding::ThisProcess,
                                          .writers = DirectoryWriters::Others } };
    auto const judged = JudgeStateDirectory(empty.Path(), guard);
    REQUIRE_FALSE(judged.has_value());
    CHECK_FALSE(judged.error().message.contains("answer this BEFORE running that"));
    CHECK(guard.Asked().empty());
}

TEST_CASE("A state directory is judged by who may write in it before who wrote what is in it",
          "[node][identity][key][secret][state]")
{
    // The writers are the cause: a foreign file in an open directory is what the open directory
    // let in, so the refusal names the directory -- and its remedy -- first.
    ScratchDirectory const scratch { "node-state-writers-first" };
    WriteBytes(scratch.Path() / "formation", std::as_bytes(std::span { std::string_view { "planted" } }));
    auto guard = ScriptedNodeKeyGuard { { .found = SecretExposure::None,
                                          .afterProtect = SecretExposure::None,
                                          .owner = FileOwnerStanding::ThisProcess,
                                          .writers = DirectoryWriters::Others } };
    guard.OwnedByAnother("formation");
    auto const judged = JudgeStateDirectory(scratch.Path(), guard);
    REQUIRE_FALSE(judged.has_value());
    CHECK(judged.error().fault == NodeKeyFault::OpenDirectory);
    CHECK(guard.Owners().empty());
}

TEST_CASE("A key planted in a directory other accounts can write in is refused for the directory, before it is read",
          "[node][identity][key][secret][state]")
{
    // What the open directory let in is not opened at all: a planted key that is no file -- here a
    // directory, which every platform can make -- is refused for the directory that admitted it,
    // not for what it is. Read first, it would be `NotARegularFile` and the remedy the wrong one.
    ScratchDirectory const scratch { "node-key-judged-first" };
    std::filesystem::create_directories(scratch.Path() / NodeKeyFileName);
    ScriptedNodeKeyGuard guard { { .found = SecretExposure::None,
                                   .afterProtect = SecretExposure::None,
                                   .owner = FileOwnerStanding::ThisProcess,
                                   .writers = DirectoryWriters::Others } };
    ScriptedSecureRandom random { ScriptedSecureRandom::Ascending(Ed25519SeedBytes) };

    auto const resolved = ResolveNodeKey(scratch.Path(), random, guard);
    REQUIRE_FALSE(resolved.has_value());
    CHECK(resolved.error().fault == NodeKeyFault::OpenDirectory);
    CHECK(random.FillCount() == 0);
    CHECK(std::filesystem::is_directory(scratch.Path() / NodeKeyFileName));
}

#if defined(_WIN32)
TEST_CASE("A key that inherited a broad read in a directory that inherits broad writes is asked about with the directory",
          "[node][identity][key][secret]")
{
    // The live `C:\ProgramData\fastcache-node\cluster` shape, on the real filesystem: the
    // directory inherits `%ProgramData%`'s `Users` create rights, and a key written there before
    // the key was minted protected inherits `Users` read. Refused for the directory -- with the
    // key's question in the same refusal, since the directory's remedy erases the evidence.
    ScratchDirectory const scratch { "node-key-inherited" };
    auto const programData = scratch.Path() / "programdata";
    std::filesystem::create_directories(programData);
    REQUIRE(Testing::ApplyAccessList(programData,
                                     L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;OW)"
                                     L"(A;OICIIO;FA;;;CO)(A;OICI;0x1200a9;;;BU)(A;CI;0x116;;;BU)"));
    auto const stateDir = programData / "cluster";
    std::filesystem::create_directories(stateDir);
    auto const key = stateDir / NodeKeyFileName;
    WriteBytes(key, ValidKeyFile(0x53));
    REQUIRE(Testing::AccessListOf(key).contains("ID;"));

    FileTrustNodeKeyGuard guard;
    ScriptedSecureRandom random { ScriptedSecureRandom::Ascending(Ed25519SeedBytes) };
    auto const resolved = ResolveNodeKey(stateDir, random, guard);
    REQUIRE_FALSE(resolved.has_value());
    CHECK(resolved.error().fault == NodeKeyFault::OpenDirectory);
    CHECK(resolved.error().message.contains("answer this BEFORE running that"));
    CHECK(resolved.error().message.contains(key.string()));
    for (auto const& command: DirectoryWritersRemedy(stateDir, DirectoryWriters::Others))
        CHECK(resolved.error().message.contains(command));
    CHECK(random.FillCount() == 0);
}
#endif

#if !defined(_WIN32)
TEST_CASE("A FIFO planted where the key goes is refused at once, after the directory is judged",
          "[node][identity][key][secret][state]")
{
    // An ordinary open of a FIFO blocks until somebody opens it for writing: the start would never
    // end, before the judgement it was about to make. So the key is opened without blocking and
    // refused as what it is. Bounded, and released if the defect is back, so a regression is a
    // red case rather than a hung binary.
    ScratchDirectory const scratch { "node-key-fifo" };
    auto const fifo = scratch / NodeKeyFileName;
    REQUIRE(::mkfifo(fifo.c_str(), 0600) == 0);
    auto guard = ScriptedNodeKeyGuard::OwnerOnly();
    ScriptedSecureRandom random { ScriptedSecureRandom::Ascending(Ed25519SeedBytes) };

    auto answered = std::atomic<bool> { false };
    auto resolved = std::optional<std::expected<NodeKey, NodeKeyRefusal>> {};
    auto resolver = std::thread { [&] {
        resolved = ResolveNodeKey(scratch.Path(), random, guard);
        answered.store(true);
    } };
    auto const prompt = Testing::WaitUntil(
        "the key resolution to answer over a FIFO",
        [&answered] { return answered.load(); },
        [] { return std::string { "still opening the FIFO" }; });
    if (!prompt)
    {
        // Release a reader blocked in its open, so the case ends rather than hangs.
        auto const writer = ::open(fifo.c_str(), O_WRONLY | O_NONBLOCK);
        if (writer >= 0)
            ::close(writer);
    }
    resolver.join();
    CHECK(prompt);

    REQUIRE(resolved.has_value());
    if (resolved.has_value())
    {
        auto const& result = *resolved;
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().fault == NodeKeyFault::NotARegularFile);
        CHECK(result.error().message.contains(fifo.string()));
        CHECK(result.error().message.contains("not a regular file"));
    }
    CHECK(random.FillCount() == 0);
    // The directory was judged FIRST: its writers were asked before the key was touched.
    CHECK(guard.Directories().size() == 1);
}
#endif

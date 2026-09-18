// SPDX-License-Identifier: Apache-2.0
#include "NodeIdentity.hpp"
#include "NodeKey.hpp"

#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/ISecureRandom.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include <tests/ScratchPath.hpp>
#include <tests/SecureRandomFakes.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Testing::ScratchDirectory;
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
/// @return The key.
[[nodiscard]] NodeKey Resolved(std::filesystem::path const& dir, ISecureRandom& random)
{
    auto resolved = ResolveNodeKey(dir, random);
    REQUIRE(resolved.has_value());
    return *std::move(resolved);
}

/// Resolve, requiring a REFUSAL, and return it.
/// @param dir The state directory.
/// @param random Where a mint's seed would come from.
/// @return The refusal.
[[nodiscard]] NodeKeyRefusal Refused(std::filesystem::path const& dir, ISecureRandom& random)
{
    // Copied out of a const local rather than moved, for a compiler rather than for taste:
    // `std::move(resolved).error()` made g++ 14.3 at -O3 -- the gate's gcc-release leg -- report
    // `-Wfree-nonheap-object` inside the expected's destructor, on the key's arm, which cannot
    // run here. This spelling builds clean there; the copy is one short string.
    auto const resolved = ResolveNodeKey(dir, random);
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
    cfg.schedulers = { "127.0.0.1:6674" };
    cfg.raftListen = "6680";
    cfg.raftSelf = "10.0.0.7";
    cfg.clusterDir = dir;
    cfg.clusterKeyFile = dir / "cluster.key";
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

    ScriptedSecureRandom first { ScriptedSecureRandom::Ascending(Ed25519SeedBytes, 0x10) };
    auto const minted = Resolved(scratch.Path(), first);
    CHECK(minted.origin == NodeKeyOrigin::Minted);
    auto const onDisk = BytesOf(scratch.Path() / NodeKeyFileName);
    CHECK(onDisk.size() == NodeKeyFileBytes);

    ScriptedSecureRandom second { ScriptedSecureRandom::Ascending(Ed25519SeedBytes, 0x80) };
    auto const readBack = Resolved(scratch.Path(), second);
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
    auto const stateDir = scratch.Path() / "state";

    ScriptedSecureRandom first { ScriptedSecureRandom::Ascending(Ed25519SeedBytes, 0x01) };
    auto const before = Resolved(stateDir, first).pair.PublicKey();

    auto removed = std::error_code {};
    std::filesystem::remove_all(stateDir, removed);
    REQUIRE_FALSE(removed);

    ScriptedSecureRandom second { ScriptedSecureRandom::Ascending(Ed25519SeedBytes, 0x40) };
    auto const after = Resolved(stateDir, second);
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
    auto const path = scratch.Path() / NodeKeyFileName;
    auto const whole = ValidKeyFile(0x20);

    for (auto const length: { std::size_t { 0 }, std::size_t { 3 }, std::size_t { 40 }, NodeKeyFileBytes - 1 })
    {
        auto const truncated = std::span<std::byte const> { whole }.first(length);
        WriteBytes(path, truncated);

        ScriptedSecureRandom random { ScriptedSecureRandom::Ascending(Ed25519SeedBytes, 0x90) };
        auto const refusal = Refused(scratch.Path(), random);
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
    auto const path = scratch.Path() / NodeKeyFileName;

    auto const refusalFor = [&](std::vector<std::byte> const& bytes) {
        WriteBytes(path, bytes);
        ScriptedSecureRandom random { ScriptedSecureRandom::Ascending(Ed25519SeedBytes) };
        auto const refusal = Refused(scratch.Path(), random);
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

TEST_CASE("A key file that cannot be opened is refused, not treated as absent", "[node][identity][key]")
{
    // ABSENT is what the open says and nothing else; an open that fails any other way, read
    // as absent, would mint over a key this machine holds. A DIRECTORY where the file should
    // be is an open that fails on every platform without needing a permission this test
    // cannot drop when it runs as root.
    ScratchDirectory const scratch { "node-key-unreadable" };
    std::filesystem::create_directories(scratch.Path() / NodeKeyFileName);

    ScriptedSecureRandom random { ScriptedSecureRandom::Ascending(Ed25519SeedBytes) };
    auto const refusal = Refused(scratch.Path(), random);
    CHECK(refusal.fault == NodeKeyFault::Unreadable);
    CHECK(random.FillCount() == 0);
    CHECK(std::filesystem::is_directory(scratch.Path() / NodeKeyFileName));
}

TEST_CASE("A key the operating system cannot draw leaves nothing behind", "[node][identity][key]")
{
    // #1527's rule arriving at the key: refused by name, never filled from a weaker source, and
    // drawn BEFORE anything is created, so the directory is not even made.
    ScratchDirectory const scratch { "node-key-draw" };
    auto const stateDir = scratch.Path() / "state";

    ScriptedSecureRandom denied { ScriptedSecureRandom::DeniedFailure() };
    auto const refusal = Refused(stateDir, denied);
    CHECK(refusal.fault == NodeKeyFault::DrawFailed);
    CHECK(refusal.message.contains("scripted-getrandom"));
    CHECK(denied.FillCount() == 1);
    CHECK_FALSE(std::filesystem::exists(stateDir));
}

#if !defined(_WIN32)
TEST_CASE("A minted key file is readable by its owner and nobody else", "[node][identity][key]")
{
    // Created mode 0600 by the call that creates it, so there is no moment in which the secret
    // is readable by another account -- a `chmod` afterwards would leave one.
    ScratchDirectory const scratch { "node-key-mode" };
    ScriptedSecureRandom random { ScriptedSecureRandom::Ascending(Ed25519SeedBytes) };
    auto const minted = Resolved(scratch.Path(), random);
    REQUIRE(minted.origin == NodeKeyOrigin::Minted);

    using std::filesystem::perms;
    auto const mode = std::filesystem::status(scratch.Path() / NodeKeyFileName).permissions();
    CHECK((mode & (perms::group_all | perms::others_all)) == perms::none);
    CHECK((mode & perms::owner_read) == perms::owner_read);
}
#endif

TEST_CASE("A node holds a key exactly when it has a state directory to keep one in", "[node][identity][key]")
{
    ScratchDirectory const scratch { "node-key-holds" };

    // Consensus always has a state directory -- named here.
    auto const clustered = ClusteredNode(scratch.Path());
    CHECK(HoldsNodeKey(clustered));
    CHECK(NodeKeyPath(clustered) == scratch.Path() / NodeKeyFileName);

    // A worker that names one holds a key in it.
    NodeConfig worker;
    worker.clusterDir = scratch.Path() / "worker";
    CHECK(HoldsNodeKey(worker));
    CHECK(NodeKeyPath(worker) == scratch.Path() / "worker" / NodeKeyFileName);

    // One that names none has nowhere a key would survive a restart, holds none, and the
    // resolution touches nothing: no directory is made and nothing is drawn.
    NodeConfig bare;
    CHECK_FALSE(HoldsNodeKey(bare));
    CHECK(NodeKeyPath(bare).empty());

    ScriptedSecureRandom random { ScriptedSecureRandom::Ascending(Ed25519SeedBytes) };
    auto const none = ResolveNodeKeyFor(bare, random);
    REQUIRE(none.has_value());
    CHECK_FALSE(none->has_value());
    CHECK(random.FillCount() == 0);

    // And the one that does, through the same call, mints where `NodeKeyPath` says.
    auto const held = ResolveNodeKeyFor(worker, random);
    REQUIRE(held.has_value());
    REQUIRE(held->has_value());
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

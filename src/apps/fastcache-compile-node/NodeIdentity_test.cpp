// SPDX-License-Identifier: Apache-2.0
#include "NodeFormation.hpp"
#include "NodeIdentity.hpp"
#include "NodeSurfaces.hpp"

#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/ISecureRandom.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <tests/HostNamingFakes.hpp>
#include <tests/NodeFormationFakes.hpp>
#include <tests/ScratchPath.hpp>
#include <tests/SecureRandomFakes.hpp>
#include <tests/UnreadablePath.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Testing::ScratchDirectory;
using FastCache::Testing::ScriptedSecureRandom;
using FastCache::Testing::Unwrap;

namespace
{

/// A node that runs consensus, minus the identity this file is about.
/// @param dir Where its consensus state lives.
/// @return The configuration.
[[nodiscard]] NodeConfig ClusteredNode(std::filesystem::path const& dir)
{
    auto cfg = Testing::FirstStart(NodeConfig {});
    cfg.schedulers = { "127.0.0.1:6674" };
    cfg.raftListen = "6680";
    cfg.raftSelf = "10.0.0.7";
    cfg.clusterDir = dir;
    return cfg;
}

/// What the identity file holds right now, or nothing when there is none.
/// @param dir The state directory.
/// @return The recorded text, verbatim.
[[nodiscard]] std::string RecordedIn(std::filesystem::path const& dir)
{
    // The sized read rather than `std::istreambuf_iterator`, for the reason
    // `ReadIdentityFile` gives in full: GCC 14 at `-O3` reports
    // `-Werror=null-dereference` inside `<streambuf>` for the iterator form, and a test
    // file is compiled by the reference build exactly as the production one is. Found
    // by AUDIT rather than by the build -- the compile stopped at the production site,
    // and this sibling was next in line behind it.
    auto stream = std::ifstream { dir / NodeIdentityFileName, std::ios::binary | std::ios::ate };
    if (!stream.is_open())
        return {};

    auto const size = stream.tellg();
    if (size < 0)
        return {};
    stream.seekg(0, std::ios::beg);

    std::string text(static_cast<std::size_t>(size), '\0');
    if (!text.empty() && !stream.read(text.data(), size))
        return {};
    return text;
}

/// The bytes a mint reads to render @p high and then @p low as its hex, big-endian.
/// @param high The first 64 bits of the id.
/// @param low The last 64 bits of the id.
/// @return The sixteen bytes.
[[nodiscard]] std::vector<std::byte> IdScript(std::uint64_t high, std::uint64_t low)
{
    std::vector<std::byte> bytes;
    for (auto const half: { high, low })
        for (auto const shift: { 56U, 48U, 40U, 32U, 24U, 16U, 8U, 0U })
            bytes.push_back(static_cast<std::byte>((half >> shift) & 0xFFU));
    return bytes;
}

/// Resolve, requiring success, so a case reads as the property it is about.
///
/// `Testing::Unwrap` takes a `std::optional`; this returns `std::expected`, and the
/// difference is deliberate at the source -- a resolution that failed has a REASON,
/// and the cases below that assert failure assert on that reason. So the helper is
/// local and narrow rather than a second `Unwrap` overload nothing else wants.
/// @param dir The state directory.
/// @param configured What `--node-id` said, or empty.
/// @param random Where a mint's bits come from.
/// @return The identity.
[[nodiscard]] NodeIdentity Resolved(std::filesystem::path const& dir, std::string_view configured, ISecureRandom& random)
{
    auto resolved = ResolveNodeIdentity(dir, configured, random);
    REQUIRE(resolved.has_value());
    return *std::move(resolved);
}

} // namespace

TEST_CASE("A node mints an identity once and reads it back forever", "[node][identity]")
{
    // **The property the whole ticket is about** (#1024). An operator no longer invents
    // a name per machine and types it twice; the node has one, it is durable, and it is
    // what the cluster admits.
    //
    // Two starts, and the SECOND is the assertion. A test that only checked "an id came
    // back" would pass against a design that minted a fresh one every time -- which is
    // the failure with no symptom here, because a re-minted id looks exactly like an id
    // until somebody notices the cluster is counting a member that no longer exists.
    ScratchDirectory const scratch { "node-identity" };

    ScriptedSecureRandom first { IdScript(0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL) };
    auto const minted = Resolved(scratch.Path(), {}, first);
    CHECK(minted.origin == NodeIdentityOrigin::Minted);
    CHECK(minted.id.size() == MintedNodeIdLength);

    // A DIFFERENT source for the second start, and that is what makes this a test. With
    // the same script a re-mint would produce the same string and the assertion below
    // would hold for the wrong reason -- the exact shape of a green test that could not
    // fail. This source would mint something else if anything asked it to.
    ScriptedSecureRandom second { IdScript(0xAAAAAAAAAAAAAAAAULL, 0xBBBBBBBBBBBBBBBBULL) };
    auto const readBack = Resolved(scratch.Path(), {}, second);
    CHECK(readBack.id == minted.id);
    CHECK(readBack.origin == NodeIdentityOrigin::Recorded);

    // And it was READ rather than re-derived, which the equality above cannot show on
    // its own: the source was never consulted.
    CHECK(second.FillCount() == 0);

    // Delete the record and the id changes. This is the acceptance clause's own
    // red-check written down: if the identity were derived from the machine rather than
    // minted, this would come back EQUAL and the case above would be passing for a
    // reason that has nothing to do with the file.
    //
    // It is also the safe direction rather than an accident. The state directory is the
    // Raft log and the vote record; a node that came back under its old identity having
    // forgotten which term it voted in is `--cluster-dir`'s own documented hazard, two
    // leaders in one term, made automatic.
    std::filesystem::remove(scratch.Path() / NodeIdentityFileName);
    ScriptedSecureRandom third { IdScript(0xAAAAAAAAAAAAAAAAULL, 0xBBBBBBBBBBBBBBBBULL) };
    auto const afresh = Resolved(scratch.Path(), {}, third);
    CHECK(afresh.id != minted.id);
    CHECK(afresh.origin == NodeIdentityOrigin::Minted);
}

TEST_CASE("Two nodes on one machine get different identities", "[node][identity]")
{
    // **The case that distinguishes this design from deriving the id from the machine**
    // -- `/etc/machine-id`, `MachineGuid`, `IOPlatformUUID`. Running several nodes on
    // one machine is a documented shape, one per toolchain, and every one of those
    // designs gives them one identity between them.
    //
    // It costs no discriminator to invent, which is the other half: two nodes on one
    // machine already need two state directories, because two Raft logs cannot share
    // one.
    ScratchDirectory const scratch { "node-identity-pair" };
    auto const left = scratch.Path() / "left";
    auto const right = scratch.Path() / "right";

    // ONE source, drawn from twice. Two sources would make this pass on the scripts
    // rather than on the design.
    SystemSecureRandom random;
    auto const first = Resolved(left, {}, random);
    auto const second = Resolved(right, {}, random);

    CHECK(first.id != second.id);
    CHECK(first.origin == NodeIdentityOrigin::Minted);
    CHECK(second.origin == NodeIdentityOrigin::Minted);

    // And each is stable in its own directory, which is what makes them two nodes
    // rather than two draws.
    CHECK(Resolved(left, {}, random).id == first.id);
    CHECK(Resolved(right, {}, random).id == second.id);
}

TEST_CASE("An operator's --node-id wins and is recorded", "[node][identity]")
{
    // The flag stays, as an override -- and recording it is what makes it an override
    // rather than a thing to keep typing. A start that took the flag and did not write
    // it down would leave the next start, made by a service whose registration somebody
    // edited, resolving something else entirely.
    ScratchDirectory const scratch { "node-identity-override" };
    SystemSecureRandom random;

    auto const typed = Resolved(scratch.Path(), "n1", random);
    CHECK(typed.id == "n1");
    CHECK(typed.origin == NodeIdentityOrigin::Configured);
    CHECK(RecordedIn(scratch.Path()) == "n1\n");

    // The half that fails if it were only ever a runtime override: a later start with
    // no flag keeps it.
    auto const later = Resolved(scratch.Path(), {}, random);
    CHECK(later.id == "n1");
    CHECK(later.origin == NodeIdentityOrigin::Recorded);

    // And an operator who changes their mind is obeyed, and the change is recorded --
    // otherwise the flag would appear to work for one start and revert at the next.
    auto const renamed = Resolved(scratch.Path(), "n2", random);
    CHECK(renamed.id == "n2");
    CHECK(renamed.origin == NodeIdentityOrigin::Configured);
    CHECK(Resolved(scratch.Path(), {}, random).id == "n2");
}

TEST_CASE("A recorded identity that cannot be read is refused, never replaced", "[node][identity]")
{
    // Re-minting over an unreadable record is the tempting repair and it is the wrong
    // one: this node may already be an admitted member, and replacing its identity
    // because a file was hard to read makes it a member the cluster has never heard of
    // while the one it counts is gone. There is no symptom -- both nodes are up.
    //
    // So it REFUSES, and the message says what deleting the file would cost.
    ScratchDirectory const scratch { "node-identity-damaged" };
    SystemSecureRandom random;
    auto const path = scratch.Path() / NodeIdentityFileName;
    std::filesystem::create_directories(scratch.Path());

    SECTION("an empty record")
    {
        std::ofstream { path, std::ios::binary | std::ios::trunc } << "\n";
        auto const refused = ResolveNodeIdentity(scratch.Path(), {}, random);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().message.contains("is empty"));
        CHECK(refused.error().fault == NodeIdentityFault::Empty);
        CHECK(StageOf(refused.error().fault) == StartStage::NodeIdentity);
    }

    SECTION("a record that is not text")
    {
        std::ofstream { path, std::ios::binary | std::ios::trunc } << "n1\x80\x80\n";
        auto const refused = ResolveNodeIdentity(scratch.Path(), {}, random);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().message.contains("does not hold text"));
        CHECK(refused.error().fault == NodeIdentityFault::NotText);
    }

    SECTION("a record that is there and cannot be read")
    {
        // Neither absent nor a verdict: nothing was read. It used to answer "absent", and the
        // resolver then MINTED -- replacing, the moment it could write, an identity the cluster
        // may have admitted. Refused instead, as the I/O failure it is, and left as it was.
        Testing::UnreadablePath const held { path, "n1\n" };
        if (!held.Held())
            SKIP("this process reads the file anyway (root), so the read arm cannot be reached here");
        auto const refused = ResolveNodeIdentity(scratch.Path(), {}, random);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().fault == NodeIdentityFault::Unreadable);
        CHECK(StageOf(refused.error().fault) == StartStage::NodeIdentityIo);
        CHECK(held.Held());
        CHECK_FALSE(std::filesystem::exists(std::filesystem::path { path }.concat(".new")));
    }

    // The control, and it is not decoration: without it "refuses a damaged record" and
    // "refuses every record" are one passing test.
    SECTION("a record with the trailing newline a text file is written with")
    {
        std::ofstream { path, std::ios::binary | std::ios::trunc } << "  n1\r\n";
        CHECK(Resolved(scratch.Path(), {}, random).id == "n1");
    }
}

TEST_CASE("An identity fault ends a one-shot command by its stage: an I/O arm transient, a verdict a decision",
          "[node][identity][exit]")
{
    // The OTHER reader of the same faults: `--print-identity` and `--install-service` are read by an
    // operator, and a retry may get past an I/O arm (1) while a verdict on the bytes read is a
    // decision (2).
    struct FaultEnding
    {
        NodeIdentityFault fault;
        CommandEnding ending;
    };
    constexpr auto verdicts = std::to_array<FaultEnding>({
        { .fault = NodeIdentityFault::Unreadable, .ending = CommandEnding::Failed },
        { .fault = NodeIdentityFault::Empty, .ending = CommandEnding::Declined },
        { .fault = NodeIdentityFault::NotText, .ending = CommandEnding::Declined },
        { .fault = NodeIdentityFault::DrawFailed, .ending = CommandEnding::Failed },
        { .fault = NodeIdentityFault::CreateFailed, .ending = CommandEnding::Failed },
        { .fault = NodeIdentityFault::WriteFailed, .ending = CommandEnding::Failed },
    });
    for (auto const fault: Enumerators<NodeIdentityFault>())
        CHECK(std::ranges::count(verdicts, fault, &FaultEnding::fault) == 1);
    for (auto const& verdict: verdicts)
    {
        INFO(static_cast<int>(verdict.fault));
        CHECK(EndingOf(verdict.fault) == verdict.ending);
    }
}

TEST_CASE("An identity fault ends a start as a refusal only when it is a verdict on bytes that were read",
          "[node][identity][exit]")
{
    struct FaultVerdict
    {
        NodeIdentityFault fault;
        StartStage stage;
    };
    constexpr auto verdicts = std::to_array<FaultVerdict>({
        { .fault = NodeIdentityFault::Unreadable, .stage = StartStage::NodeIdentityIo },
        { .fault = NodeIdentityFault::Empty, .stage = StartStage::NodeIdentity },
        { .fault = NodeIdentityFault::NotText, .stage = StartStage::NodeIdentity },
        { .fault = NodeIdentityFault::DrawFailed, .stage = StartStage::NodeIdentityIo },
        { .fault = NodeIdentityFault::CreateFailed, .stage = StartStage::NodeIdentityIo },
        { .fault = NodeIdentityFault::WriteFailed, .stage = StartStage::NodeIdentityIo },
    });
    for (auto const fault: Enumerators<NodeIdentityFault>())
        CHECK(std::ranges::count(verdicts, fault, &FaultVerdict::fault) == 1);
    for (auto const& verdict: verdicts)
    {
        INFO(static_cast<int>(verdict.fault));
        CHECK(StageOf(verdict.fault) == verdict.stage);
    }
    CHECK(ExitOf(StartStage::NodeIdentity) == ProcessExit::Refused);
    CHECK(ExitOf(StartStage::NodeIdentityIo) == ProcessExit::Failed);
}

TEST_CASE("A minted identity is 128 bits of the source it was given", "[node][identity]")
{
    // Fresh per mint, and drawn rather than derived. The draws are scripted so the
    // rendering is pinned as well as the length -- a mint that silently truncated, or
    // that folded the two halves together, would still produce something 32 characters
    // long.
    ScriptedSecureRandom random { IdScript(0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL) };
    CHECK(MintNodeId(random).value() == "0123456789abcdeffedcba9876543210");
    CHECK(random.FillCount() == 1);

    // All sixteen bytes, not eight: one 64-bit half rendered twice would pass a length
    // check and halve the space.
    ScriptedSecureRandom halves { IdScript(1, 2) };
    auto const id = MintNodeId(halves).value();
    CHECK(id.substr(0, 16) != id.substr(16));
}

TEST_CASE("An identity the generator cannot draw is refused by name, and nothing is written", "[node][identity]")
{
    // No fallback to a weaker source, and no half-made state: the refusal names the seam's own
    // failure, and the state directory a start would have created is not there (#1527). A node
    // that cannot mint refuses to start rather than run under an id two machines could share.
    ScratchDirectory const scratch { "node-identity-no-entropy" };
    auto const state = scratch.Path() / "state";
    ScriptedSecureRandom denied { ScriptedSecureRandom::DeniedFailure() };

    auto const minted = MintNodeId(denied);
    REQUIRE_FALSE(minted.has_value());
    CHECK(minted.error().primitive == ScriptedSecureRandom::DeniedFailure().primitive);

    auto const refused = ResolveNodeIdentity(state, {}, denied);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().message.contains("cannot mint an identity"));
    CHECK(refused.error().message.contains(ScriptedSecureRandom::DeniedFailure().primitive));
    // A draw that failed is an I/O arm: the next start may draw.
    CHECK(refused.error().fault == NodeIdentityFault::DrawFailed);
    CHECK(StageOf(refused.error().fault) == StartStage::NodeIdentityIo);
    CHECK_FALSE(std::filesystem::exists(state));
    CHECK(denied.FillCount() == 2);

    // The controls: an identity that needs no draw is not refused for want of one, and the
    // source is not even asked -- so the refusal above is the MINT's, not the resolver's.
    CHECK(Resolved(state, "n1", denied).id == "n1");
    CHECK(Resolved(state, {}, denied).origin == NodeIdentityOrigin::Recorded);
    CHECK(denied.FillCount() == 2);
}

TEST_CASE("The state directory has one author, and the default names no identity", "[node][identity]")
{
    // `ConsensusTier::Start` was the second author of this default until #1024, and it
    // had to change with the identity: the old one was `fastcache-cluster/<node-id>`,
    // and an identity READ OUT of the state directory cannot name the directory it is
    // read from. The default is the platform's now, resolved at startup -- into scratch
    // here, through the scripted probe -- and it names no identity either.
    ScratchDirectory const scratch { "node-identity-default" };
    auto bare = Testing::FirstStart(NodeConfig {});
    ApplyNodeStateDirectory(bare,
                            Testing::ScriptedConfigPathProbe { { { "LOCALAPPDATA", scratch.Path().string() },
                                                                 { "XDG_STATE_HOME", scratch.Path().string() } },
                                                               Testing::ScriptedConfigPathProbe::Privilege::Unprivileged });
    CHECK(NodeStateDirectory(bare) == scratch.Path() / NodeStateDirectoryName);

    // Which is what makes the flag the discriminator, and the discriminator was never
    // lost: two nodes on one machine need two Raft logs and so two directories.
    auto named = Testing::FirstStart(NodeConfig {});
    named.clusterDir = std::filesystem::path { "/var/lib/fastcache-node/left" };
    CHECK(NodeStateDirectory(named) == named.clusterDir);
}

TEST_CASE("A --raft-self host becomes this node's own bootstrap member", "[node][identity][consensus]")
{
    // **Not separable ergonomics.** A minted identity is typed nowhere, and a node that
    // names no member it can be dialled at is refused -- so without this a derived identity
    // is unusable.
    ScratchDirectory const scratch { "node-identity-self" };
    SystemSecureRandom random;

    auto cfg = ClusteredNode(scratch.Path());
    auto const identity = Resolved(NodeStateDirectory(cfg), cfg.nodeId, random);
    ApplyNodeIdentity(cfg, identity);

    CHECK(cfg.nodeId == identity.id);
    auto const members = BootstrapMembersOf(cfg);
    REQUIRE(members.size() == 1);
    CHECK(members.front().id == identity.id);

    // The HOST from `--raft-self` and the PORT from `--listen-raft`, which is the whole
    // reason the flag takes only a host: a bare `--listen-raft` binds the WILDCARD, so
    // the address this node binds is not one any peer could dial. A member entry naming
    // `0.0.0.0` is a member nobody can reach.
    CHECK(members.front().raftEndpoint == "10.0.0.7:6680");
    CHECK(RowFor(NodeSurface::Raft).Resolve(cfg).front().host == "0.0.0.0");

    // And the startup table accepts what this produces, which is the half that would
    // otherwise be asserted about a configuration the node refuses to start in.
    CHECK_FALSE(StartupPolicyRejection(cfg).has_value());
}

TEST_CASE("A consensus node that names itself neither way is refused", "[node][identity][consensus]")
{
    // The rule reads the config as PARSED, before `ApplyNodeIdentity` gives it an id --
    // which is what keeps it a pure function of the command line and lets
    // `--install-service` reach it.
    //
    // Both directions: `--raft-self` satisfies it, and its absence does not -- once the host
    // name it would otherwise fall back to has resolved to nothing. Before it resolves the
    // address is awaited, not missing.
    auto neither = Testing::FirstStart(NodeConfig {});
    neither.schedulers = { "127.0.0.1:6674" };
    // Where CLIENTS dial it is stated, so the only address left unnamed is the one this rule is
    // about: a consensus node admits other machines by key, and with no host name resolved an
    // unnamed advertise would be the wildcard, which its own row refuses first.
    neither.advertise = "10.0.0.7:6674";
    neither.raftListen = "6680";
    ApplyHostNames(neither, NodeHostNames { .fqdn = {}, .dnsSuffix = {}, .withheld = {} });
    auto const refusal = StartupPolicyRejection(neither);
    REQUIRE(refusal.has_value());
    CHECK(Unwrap(refusal) == ConsensusNamesNoDialAddressRefusal);

    auto named = neither;
    named.raftSelf = "10.0.0.7";
    CHECK_FALSE(StartupPolicyRejection(named).has_value());
}

TEST_CASE("The startup row refuses exactly the consensus nodes whose dial address nobody stated",
          "[node][identity][consensus]")
{
    // #1328: the row asks `ConsensusDialAddressOf`, the answer `--print-surfaces` prints NOT
    // STATED from and `NodeStatus` reports. Asserted as an AGREEMENT over shapes covering every
    // answer, because the spelling it replaced agreed with the derivation on every
    // configuration anybody had written down -- so an expected verdict per shape cannot tell
    // the row ASKING the derivation from the row keeping a copy of it. The expectation is its
    // own line, apart from the agreement: a change to the derivation reddens that one, and a
    // row that stopped asking reddens the agreement.
    struct Shape
    {
        std::string_view what; ///< Which configuration, for the failure message.
        NodeConfig cfg;        ///< The configuration as parsed.
        bool unstated;         ///< Whether nobody stated where peers dial it.
    };

    auto worker = Testing::FirstStart(NodeConfig {});
    worker.schedulers = { "127.0.0.1:6674" };
    // Stated for `A consensus node that names itself neither way`'s reason: the client address is
    // not what this row asks about, and a consensus shape leaving it to a withheld name is refused
    // by the wildcard row first.
    worker.advertise = "10.0.0.7:6674";
    worker.raftListen.clear();
    auto consensus = worker;
    consensus.raftListen = "6680";
    // A host name resolved to nothing, so "nobody stated where peers dial it" is reachable:
    // with none resolved yet the address is awaited instead, which is not this row's answer.
    ApplyHostNames(consensus, NodeHostNames { .fqdn = {}, .dnsSuffix = {}, .withheld = {} });

    std::vector<Shape> shapes;
    shapes.push_back({ .what = "no consensus port, whatever else is named",
                       .cfg =
                           [&worker] {
                               auto cfg = worker;
                               cfg.raftSelf = "10.0.0.7";
                               return cfg;
                           }(),
                       .unstated = false });
    shapes.push_back({ .what = "a consensus port and nothing else", .cfg = consensus, .unstated = true });
    shapes.push_back({ .what = "--raft-self",
                       .cfg =
                           [&consensus] {
                               auto cfg = consensus;
                               cfg.raftSelf = "10.0.0.7";
                               return cfg;
                           }(),
                       .unstated = false });
    std::size_t refusedByTheRow = 0;
    for (auto const& [what, cfg, unstated]: shapes)
    {
        INFO(what);
        auto const dial = ConsensusDialAddressOf(cfg);
        auto const dialUnstated = !dial.has_value() && dial.error() == ConsensusDialGap::Unstated;
        CHECK(dialUnstated == unstated);

        // WHICH refusal, by the row's own constant: a shape refused by some other row first
        // is not this row agreeing.
        auto const refused = StartupPolicyRejection(cfg).value_or(std::string {}) == ConsensusNamesNoDialAddressRefusal;
        CHECK(refused == dialUnstated);
        refusedByTheRow += refused ? 1 : 0;
    }
    // Both answers were reached, so neither half of the agreement held vacuously.
    CHECK(refusedByTheRow == 1);
}

TEST_CASE("A configuration survives having its identity applied twice", "[node][identity][consensus]")
{
    // The reload path, which is where this bites: `main` resolves once and applies the
    // result to every candidate it rebuilds, and `ValidateNodeReloadable` then runs the
    // STARTUP rules over the applied candidate. So the rules have to hold for a config
    // that has already been through `ApplyNodeIdentity` -- and a node whose reloads are
    // all refused by name is one whose operator cannot change a log level.
    ScratchDirectory const scratch { "node-identity-reload" };
    SystemSecureRandom random;

    auto cfg = ClusteredNode(scratch.Path());
    auto const identity = Resolved(NodeStateDirectory(cfg), cfg.nodeId, random);

    ApplyNodeIdentity(cfg, identity);
    CHECK_FALSE(StartupPolicyRejection(cfg).has_value());

    // Idempotent, because a second application is exactly what a reload performs on a
    // candidate rebuilt from the same file and the same argv.
    auto const membersAfterOne = BootstrapMembersOf(cfg);
    ApplyNodeIdentity(cfg, identity);
    CHECK(BootstrapMembersOf(cfg) == membersAfterOne);
    CHECK_FALSE(StartupPolicyRejection(cfg).has_value());
}

TEST_CASE("A --raft-self without a consensus port is refused", "[node][identity][consensus]")
{
    // The port is the half this flag deliberately does not carry, so without
    // `--listen-raft` there is nothing to pair the host with -- and no consensus for
    // the pair to name a member of.
    auto cfg = Testing::FirstStart(NodeConfig {});
    cfg.schedulers = { "127.0.0.1:6674" };
    cfg.raftSelf = "10.0.0.7";
    cfg.raftListen.clear();

    auto const refusal = StartupPolicyRejection(cfg);
    REQUIRE(refusal.has_value());
    CHECK(Unwrap(refusal).starts_with("--raft-self names the host"));
}

TEST_CASE("An invocation that only asks a question mints nothing", "[node][identity]")
{
    // Resolving MINTS, which creates a directory and a file. `--print-surfaces` says in
    // its own comment that it opens nothing and changes nothing; a cluster verb is a
    // client dialling somebody else's scheduler; and removing a registration is the
    // recovery an operator reaches for when the configuration is already wrong. None of
    // them is entitled to leave state behind.
    auto const clustered = [] {
        auto cfg = Testing::FirstStart(NodeConfig {});
        cfg.schedulers = { "127.0.0.1:6674" };
        cfg.raftListen = "6680";
        cfg.raftSelf = "10.0.0.7";
        return cfg;
    };

    // The control FIRST, so that "mints nothing" cannot pass because nothing ever
    // mints.
    CHECK(NodeIdentityNeed(clustered()) == IdentityNeed::Mint);

    auto surfaces = clustered();
    surfaces.printSurfaces = true;
    CHECK(NodeIdentityNeed(surfaces) == IdentityNeed::None);

    auto uninstall = clustered();
    uninstall.uninstallService = true;
    CHECK(NodeIdentityNeed(uninstall) == IdentityNeed::None);

    auto status = clustered();
    status.cluster.action = ClusterAction::Status;
    CHECK(NodeIdentityNeed(status) == IdentityNeed::None);

    // An install DOES mint, and that is the point rather than an oversight: the
    // registration bakes the resolved value in, so it has to exist by then.
    auto install = clustered();
    install.installService = true;
    CHECK(NodeIdentityNeed(install) == IdentityNeed::Mint);

    // And a node running no consensus keeps one too: every node holds an identity key now,
    // and the id travels with it -- a worker proves it on every connection to a scheduler.
    auto lone = Testing::FirstStart(NodeConfig {});
    lone.schedulers = { "127.0.0.1:6674" };
    lone.raftListen.clear();
    CHECK(NodeIdentityNeed(lone) == IdentityNeed::Mint);
}

TEST_CASE("A service registration bakes in the resolved identity", "[node][identity][service]")
{
    // **The failure with no symptom.** A registration replays its command line forever;
    // one that omitted the identity would let a re-image resolve a different one under
    // a registration nobody edited, and the cluster would count a member that no longer
    // exists beside a stranger nobody admitted. Both nodes are up the whole time.
    ScratchDirectory const scratch { "node-identity-install" };
    SystemSecureRandom random;

    auto cfg = ClusteredNode(scratch.Path());
    cfg.advertise = "10.0.0.7:6674";
    cfg.advertiseExplicit = true;
    cfg.raftListenExplicit = true;
    cfg.raftSelfExplicit = true;
    // A bind the advertised address answers on: a consensus node admits machines by key, so a
    // routable advertise over a loopback bind is refused at every boot the registration replays.
    cfg.nodeListen = "0.0.0.0:6674";
    cfg.nodeListenExplicit = true;

    auto const identity = Resolved(NodeStateDirectory(cfg), cfg.nodeId, random);
    ApplyNodeIdentity(cfg, identity);

    auto const spec =
        MakeNodeServiceSpec(std::filesystem::path { "fastcache-compile-node" }, cfg, Testing::InstallerPathProbe());

    // Read back by PARSING the registration, never by grepping it: what matters is that
    // the value survives this project's own parser and arrives in the field the node
    // reads, which a substring check cannot show.
    std::vector<char const*> argv;
    argv.reserve(spec.arguments.size());
    for (auto const& argument: spec.arguments)
        argv.push_back(argument.c_str());

    auto replayed = Testing::FirstStart(NodeConfig {});
    auto const flow = ParseOptionsInto(NodeOptions(), std::span<char const* const> { argv }, replayed);
    REQUIRE(flow.has_value());
    CHECK(replayed.nodeId == identity.id);
    CHECK(replayed.raftSelf == "10.0.0.7");

    // And the replayed command line still starts, which is the property #168 is about:
    // a registration that passes the install table and then exits at every boot is the
    // whole reason these rules are pure functions of the command line.
    replayed.clusterDir = cfg.clusterDir;
    CHECK_FALSE(StartupPolicyRejection(replayed).has_value());
}

namespace
{
/// A public key whose every byte is @p fill. The roster never asks whether it is a curve point.
/// @param fill The byte.
/// @return The key.
[[nodiscard]] Ed25519PublicKey KeyOf(std::uint8_t fill)
{
    auto key = Ed25519PublicKey {};
    key.fill(static_cast<std::byte>(fill));
    return key;
}

} // namespace

TEST_CASE("The key a node holds reaches its own member entry and the configuration", "[node][identity][key]")
{
    // #178. The record a leader announces is this node's own entry, so the key has to be ON
    // it -- and applied through the one function a reload runs too, or a candidate would hold
    // a self member whose key has changed.
    ScratchDirectory const scratch { "node-identity-key" };
    SystemSecureRandom random;

    auto cfg = ClusteredNode(scratch.Path());
    auto identity = Resolved(NodeStateDirectory(cfg), cfg.nodeId, random);
    identity.publicKey = KeyOf(0x5A);

    ApplyNodeIdentity(cfg, identity);
    CHECK(cfg.identityPublicKey == std::optional { KeyOf(0x5A) });
    auto const members = BootstrapMembersOf(cfg);
    REQUIRE(members.size() == 1);
    CHECK(members.front().publicKey == std::optional { KeyOf(0x5A) });

    // And applying twice -- what a reload does to a candidate -- changes nothing.
    ApplyNodeIdentity(cfg, identity);
    CHECK(BootstrapMembersOf(cfg) == members);
}

TEST_CASE("A node that runs no consensus still carries the key it holds", "[node][identity][key]")
{
    // A worker naming --cluster-dir holds a key and has no id. The key-only identity is what
    // reaches it -- and every reload candidate -- without inventing an id or a member entry.
    auto worker = Testing::FirstStart(NodeConfig {});
    worker.clusterDir = std::filesystem::path { "/var/lib/fastcache-node" };

    auto const keyOnly = NodeIdentity { .id = {}, .origin = NodeIdentityOrigin::Recorded, .publicKey = KeyOf(0x77) };
    ApplyNodeIdentity(worker, keyOnly);
    CHECK(worker.identityPublicKey == std::optional { KeyOf(0x77) });
    CHECK(worker.nodeId.empty());
}

TEST_CASE("What --print-identity prints is the line --cluster-admit reads back into this member", "[node][identity][key]")
{
    // #178: an operator who admits a member without an enrollment window types its key, and
    // this is where it is copied from. So the line is asserted by PARSING it through the flag,
    // the way the operator's command will be, rather than by its spelling.
    auto const key = Ed25519KeyPair::FromSeed(ScriptedSecureRandom::Ascending(Ed25519SeedBytes)).value().PublicKey();

    SECTION("a consensus member prints its id, its key and its admission line")
    {
        auto const text = DescribeIdentity("n1", key, std::optional<std::string> { "10.0.0.7:6680" }, IdentityRole::Member);
        CHECK(text.contains(std::format("node-id n1\n")));
        CHECK(text.contains(std::format("public-key {}\n", FormatEd25519PublicKey(key))));

        auto const lineAt = text.find("cluster-admit ");
        REQUIRE(lineAt != std::string::npos);
        auto const value = std::string_view { text }.substr(lineAt + std::string_view { "cluster-admit " }.size());
        auto const argument = std::format("--cluster-admit={}", value.substr(0, value.find('\n')));

        auto parsed = Testing::FirstStart(NodeConfig {});
        auto const argv = std::array { "--scheduler=10.0.0.1:6675", argument.c_str() };
        REQUIRE(ParseOptionsInto(NodeOptions(), std::span<char const* const> { argv }, parsed).has_value());
        CHECK(parsed.cluster.action == ClusterAction::Admit);
        CHECK(parsed.cluster.key == "n1");
        CHECK(parsed.cluster.value == "10.0.0.7:6680");
        CHECK(parsed.cluster.publicKey == std::optional { key });
    }

    SECTION("a node its peers could not dial yet prints no admission line rather than a guessed one")
    {
        auto const text = DescribeIdentity("n1", key, std::nullopt, IdentityRole::Member);
        CHECK(text.contains("public-key "));
        CHECK_FALSE(text.contains("cluster-admit"));
    }

    SECTION("a node with no id yet has a key and nothing else to print")
    {
        auto const text = DescribeIdentity("", key, std::nullopt, IdentityRole::Worker);
        CHECK(text == std::format("public-key {}\n", FormatEd25519PublicKey(key)));
    }
}

TEST_CASE("What --print-identity prints for a worker is what --cluster-admit-worker reads back", "[node][identity][key]")
{
    // #178 PR 6: a worker that runs no consensus is admitted by the identity it proves, and an
    // operator who does not open an enrollment window admits it by typing what it printed. So the
    // line is asserted by PARSING it through the flag, the way the operator's command will be.
    auto const key = Ed25519KeyPair::FromSeed(ScriptedSecureRandom::Ascending(Ed25519SeedBytes)).value().PublicKey();
    auto const text = DescribeIdentity("w-7", key, std::nullopt, IdentityRole::Worker);
    CHECK(text.contains("node-id w-7\n"));
    // A worker is no consensus member, so it prints no line `--cluster-admit` would read.
    CHECK_FALSE(text.contains("cluster-admit "));

    auto const lineAt = text.find("cluster-admit-worker ");
    REQUIRE(lineAt != std::string::npos);
    auto const value = std::string_view { text }.substr(lineAt + std::string_view { "cluster-admit-worker " }.size());
    auto const argument = std::format("--cluster-admit-worker={}", value.substr(0, value.find('\n')));

    auto parsed = Testing::FirstStart(NodeConfig {});
    auto const argv = std::array { "--scheduler=10.0.0.1:6675", argument.c_str() };
    REQUIRE(ParseOptionsInto(NodeOptions(), std::span<char const* const> { argv }, parsed).has_value());
    CHECK(parsed.cluster.action == ClusterAction::AdmitWorker);
    CHECK(parsed.cluster.key == "w-7");
    CHECK(parsed.cluster.publicKey == std::optional { key });

    // A member prints the other line, never this one: the two routes admit different things.
    CHECK_FALSE(DescribeIdentity("n1", key, std::optional<std::string> { "10.0.0.7:6680" }, IdentityRole::Member)
                    .contains("cluster-admit-worker"));
}

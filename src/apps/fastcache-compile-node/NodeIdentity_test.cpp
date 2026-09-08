// SPDX-License-Identifier: Apache-2.0
#include "NodeIdentity.hpp"
#include "NodeSurfaces.hpp"

#include <FastCache/Core/IRandomSource.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

#include <tests/ScratchPath.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Testing::ScratchDirectory;
using FastCache::Testing::Unwrap;

namespace
{

/// A node that runs consensus, minus the identity this file is about.
/// @param dir Where its consensus state lives.
/// @return The configuration.
[[nodiscard]] NodeConfig ClusteredNode(std::filesystem::path const& dir)
{
    NodeConfig cfg;
    cfg.scheduler = "127.0.0.1:6674";
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
[[nodiscard]] NodeIdentity Resolved(std::filesystem::path const& dir, std::string_view configured, IRandomSource& random)
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

    ScriptedRandomSource first { { 0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL } };
    auto const minted = Resolved(scratch.Path(), {}, first);
    CHECK(minted.origin == NodeIdentityOrigin::Minted);
    CHECK(minted.id.size() == MintedNodeIdLength);

    // A DIFFERENT source for the second start, and that is what makes this a test. With
    // the same script a re-mint would produce the same string and the assertion below
    // would hold for the wrong reason -- the exact shape of a green test that could not
    // fail. This source would mint something else if anything asked it to.
    ScriptedRandomSource second { { 0xAAAAAAAAAAAAAAAAULL, 0xBBBBBBBBBBBBBBBBULL } };
    auto const readBack = Resolved(scratch.Path(), {}, second);
    CHECK(readBack.id == minted.id);
    CHECK(readBack.origin == NodeIdentityOrigin::Recorded);

    // And it was READ rather than re-derived, which the equality above cannot show on
    // its own: the source was never consulted.
    CHECK(second.DrawCount() == 0);

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
    ScriptedRandomSource third { { 0xAAAAAAAAAAAAAAAAULL, 0xBBBBBBBBBBBBBBBBULL } };
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
    SystemRandomSource random { 20260908 };
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
    SystemRandomSource random { 20260908 };

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
    SystemRandomSource random { 20260908 };
    auto const path = scratch.Path() / NodeIdentityFileName;
    std::filesystem::create_directories(scratch.Path());

    SECTION("an empty record")
    {
        std::ofstream { path, std::ios::binary | std::ios::trunc } << "\n";
        auto const refused = ResolveNodeIdentity(scratch.Path(), {}, random);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().contains("is empty"));
    }

    SECTION("a record that is not text")
    {
        std::ofstream { path, std::ios::binary | std::ios::trunc } << "n1\x80\x80\n";
        auto const refused = ResolveNodeIdentity(scratch.Path(), {}, random);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().contains("does not hold text"));
    }

    // The control, and it is not decoration: without it "refuses a damaged record" and
    // "refuses every record" are one passing test.
    SECTION("a record with the trailing newline a text file is written with")
    {
        std::ofstream { path, std::ios::binary | std::ios::trunc } << "  n1\r\n";
        CHECK(Resolved(scratch.Path(), {}, random).id == "n1");
    }
}

TEST_CASE("A minted identity is 128 bits of the source it was given", "[node][identity]")
{
    // Fresh per mint, and drawn rather than derived. The draws are scripted so the
    // rendering is pinned as well as the length -- a mint that silently truncated, or
    // that folded the two halves together, would still produce something 32 characters
    // long.
    ScriptedRandomSource random { { 0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL } };
    CHECK(MintNodeId(random) == "0123456789abcdeffedcba9876543210");
    CHECK(random.DrawCount() == 2);

    // Two draws, not one: a single 64-bit value rendered twice would pass a length
    // check and halve the space.
    ScriptedRandomSource halves { { 1, 2 } };
    auto const id = MintNodeId(halves);
    CHECK(id.substr(0, 16) != id.substr(16));
}

TEST_CASE("The state directory has one author, and the default names no identity", "[node][identity]")
{
    // `ConsensusTier::Start` was the second author of this default until #1024, and it
    // had to change with the identity: the old one was `fastcache-cluster/<node-id>`,
    // and an identity READ OUT of the state directory cannot name the directory it is
    // read from.
    NodeConfig bare;
    CHECK(NodeStateDirectory(bare) == std::filesystem::path { "fastcache-cluster" });

    // Which is what makes the flag the discriminator, and the discriminator was never
    // lost: two nodes on one machine need two Raft logs and so two directories.
    NodeConfig named;
    named.clusterDir = std::filesystem::path { "/var/lib/fastcache-node/left" };
    CHECK(NodeStateDirectory(named) == named.clusterDir);
}

TEST_CASE("A --raft-self host becomes this node's own member entry", "[node][identity][consensus]")
{
    // **Not separable ergonomics.** A minted identity cannot be typed into
    // `--raft-peer=<id>=<host>:<port>`, and a node that names no member of its own
    // configuration is refused -- so without this a derived identity is unusable.
    ScratchDirectory const scratch { "node-identity-self" };
    SystemRandomSource random { 20260908 };

    auto cfg = ClusteredNode(scratch.Path());
    REQUIRE(ClusterSelfMember(cfg) == nullptr);

    auto const identity = Resolved(NodeStateDirectory(cfg), cfg.nodeId, random);
    ApplyNodeIdentity(cfg, identity);

    CHECK(cfg.nodeId == identity.id);
    auto const* const self = ClusterSelfMember(cfg);
    REQUIRE(self != nullptr);

    // The HOST from `--raft-self` and the PORT from `--listen-raft`, which is the whole
    // reason the flag takes only a host: a bare `--listen-raft` binds the WILDCARD, so
    // the address this node binds is not one any peer could dial. A member entry naming
    // `0.0.0.0` is a member nobody can reach.
    CHECK(self->raftEndpoint == "10.0.0.7:6680");
    CHECK(RowFor(NodeSurface::Raft).Resolve(cfg).front().host == "0.0.0.0");

    // And the startup table accepts what this produces, which is the half that would
    // otherwise be asserted about a configuration the node refuses to start in.
    CHECK_FALSE(StartupPolicyRejection(cfg).has_value());
}

TEST_CASE("A consensus node that names itself neither way is refused", "[node][identity][consensus]")
{
    // The rule reads the config as PARSED, before `ApplyNodeIdentity` turns the flag
    // into a member -- which is what keeps it a pure function of the command line and
    // lets `--install-service` reach it.
    //
    // Both directions: `--raft-self` satisfies it, and its absence does not.
    NodeConfig neither;
    neither.scheduler = "127.0.0.1:6674";
    neither.raftListen = "6680";
    auto const refusal = StartupPolicyRejection(neither);
    REQUIRE(refusal.has_value());
    CHECK(Unwrap(refusal) == ConsensusNamesNoSelfPeerRefusal);

    auto named = neither;
    named.raftSelf = "10.0.0.7";
    CHECK_FALSE(StartupPolicyRejection(named).has_value());
}

TEST_CASE("A --raft-self that CONTRADICTS a --raft-peer for this node is refused", "[node][identity][consensus]")
{
    // Two answers to "where do this node's peers dial it", and nothing to rank them by.
    // `--raft-self` exists for the node whose identity was derived; an operator who
    // typed the id has already said where this node answers.
    NodeConfig both;
    both.scheduler = "127.0.0.1:6674";
    both.raftListen = "6680";
    both.nodeId = "n1";
    both.raftPeers = { Unwrap(Cluster::ParseMemberSpec("n1=10.0.0.4:6680")) };
    both.raftSelf = "10.0.0.7";

    auto const refusal = StartupPolicyRejection(both);
    REQUIRE(refusal.has_value());
    CHECK(Unwrap(refusal).starts_with("--raft-self and a --raft-peer"));

    // **AGREEING is not contradicting**, and this half is not politeness: the reload
    // path judges a candidate `ApplyNodeIdentity` has already completed, so a rule that
    // refused any self peer beside `--raft-self` would accept a node at startup and
    // refuse every reload of it by name. The synthesised entry IS this shape.
    auto agreeing = both;
    agreeing.raftPeers = { Unwrap(Cluster::ParseMemberSpec("n1=10.0.0.7:6680")) };
    CHECK_FALSE(StartupPolicyRejection(agreeing).has_value());

    // Each alone is fine, which is what stops this refusing the two shapes it exists to
    // allow.
    auto typed = both;
    typed.raftSelf.clear();
    CHECK_FALSE(StartupPolicyRejection(typed).has_value());

    auto derived = both;
    derived.raftPeers.clear();
    derived.nodeId.clear();
    CHECK_FALSE(StartupPolicyRejection(derived).has_value());
}

TEST_CASE("A configuration survives having its identity applied twice", "[node][identity][consensus]")
{
    // The reload path, which is where this bites: `main` resolves once and applies the
    // result to every candidate it rebuilds, and `ValidateNodeReloadable` then runs the
    // STARTUP rules over the applied candidate. So the rules have to hold for a config
    // that has already been through `ApplyNodeIdentity` -- and a node whose reloads are
    // all refused by name is one whose operator cannot change a log level.
    ScratchDirectory const scratch { "node-identity-reload" };
    SystemRandomSource random { 20260908 };

    auto cfg = ClusteredNode(scratch.Path());
    auto const identity = Resolved(NodeStateDirectory(cfg), cfg.nodeId, random);

    ApplyNodeIdentity(cfg, identity);
    CHECK_FALSE(StartupPolicyRejection(cfg).has_value());

    // Idempotent, because a second application is exactly what a reload performs on a
    // candidate rebuilt from the same file and the same argv.
    auto const peersAfterOne = cfg.raftPeers.size();
    ApplyNodeIdentity(cfg, identity);
    CHECK(cfg.raftPeers.size() == peersAfterOne);
    CHECK_FALSE(StartupPolicyRejection(cfg).has_value());
}

TEST_CASE("A --raft-self without a consensus port is refused", "[node][identity][consensus]")
{
    // The port is the half this flag deliberately does not carry, so without
    // `--listen-raft` there is nothing to pair the host with -- and no consensus for
    // the pair to name a member of.
    NodeConfig cfg;
    cfg.scheduler = "127.0.0.1:6674";
    cfg.raftSelf = "10.0.0.7";

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
        NodeConfig cfg;
        cfg.scheduler = "127.0.0.1:6674";
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

    // And a node running no consensus has no identity to keep.
    NodeConfig lone;
    lone.scheduler = "127.0.0.1:6674";
    CHECK(NodeIdentityNeed(lone) == IdentityNeed::None);
}

TEST_CASE("A service registration bakes in the resolved identity", "[node][identity][service]")
{
    // **The failure with no symptom.** A registration replays its command line forever;
    // one that omitted the identity would let a re-image resolve a different one under
    // a registration nobody edited, and the cluster would count a member that no longer
    // exists beside a stranger nobody admitted. Both nodes are up the whole time.
    ScratchDirectory const scratch { "node-identity-install" };
    SystemRandomSource random { 20260908 };

    auto cfg = ClusteredNode(scratch.Path());
    cfg.advertise = "10.0.0.7:6674";
    cfg.schedulerExplicit = true;
    cfg.advertiseExplicit = true;
    cfg.raftListenExplicit = true;
    cfg.raftSelfExplicit = true;

    auto const identity = Resolved(NodeStateDirectory(cfg), cfg.nodeId, random);
    ApplyNodeIdentity(cfg, identity);

    auto const spec = MakeNodeServiceSpec(std::filesystem::path { "fastcache-compile-node" }, cfg);

    // Read back by PARSING the registration, never by grepping it: what matters is that
    // the value survives this project's own parser and arrives in the field the node
    // reads, which a substring check cannot show.
    std::vector<char const*> argv;
    argv.reserve(spec.arguments.size());
    for (auto const& argument: spec.arguments)
        argv.push_back(argument.c_str());

    NodeConfig replayed;
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

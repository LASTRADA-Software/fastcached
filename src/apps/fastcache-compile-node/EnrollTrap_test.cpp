// SPDX-License-Identifier: Apache-2.0
#include "EnrollClient.hpp"
#include "NodeConfig.hpp"
#include "NodeCredential.hpp"

#include <FastCache/Consensus/FileRaftStorage.hpp>
#include <FastCache/Consensus/RaftTypes.hpp>
#include <FastCache/Core/IRandomSource.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <vector>

#include <tests/ScratchPath.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Testing::Unwrap;

namespace
{

/// Write what a node that bootstrapped a cluster of ITSELF leaves behind.
///
/// Not a hand-rolled file: `FileRaftStorage` is the one writer of this format in the
/// tree, so a fixture that composed bytes would be asserting against its own idea of
/// the layout rather than against the one production writes. What is staged is what
/// `RaftNode::BecomeLeader` records -- a term, a self-vote, and the no-op entry it
/// appends on winning.
/// @param directory Where `--cluster-dir` points.
/// @param self The identity this node elected.
void StageSelfElected(std::filesystem::path const& directory, std::string self)
{
    auto storage = Consensus::FileRaftStorage::Open(directory);
    REQUIRE(storage.has_value());

    // The move is taken OUTSIDE the assertion, and that is not a style preference:
    // `REQUIRE` expands its argument more than once -- once to evaluate and once to
    // reconstruct the expression for the failure message -- so a `std::move` written
    // inside it reads to the analyser as a use after a move
    // (`bugprone-use-after-move`), and to a reader as one too.
    auto const stateSaved = storage->SaveState(Consensus::PersistentState {
        .currentTerm = Consensus::Term { .value = 1 }, .votedFor = Consensus::NodeId { std::move(self) } });
    REQUIRE(stateSaved.has_value());
    REQUIRE(storage
                ->SaveLog(Consensus::LogAppend { .fromIndex = Consensus::LogIndex { .value = 1 },
                                                 .entries = { Consensus::LogEntry { .term = Consensus::Term { .value = 1 },
                                                                                    .kind = Consensus::EntryKind::NoOp,
                                                                                    .payload = {} } } })
                .has_value());
}

} // namespace

TEST_CASE("A state directory that has never run consensus reports no history", "[enrollment][trap]")
{
    Testing::ScratchDirectory scratch { "enroll-trap-fresh" };

    // **The case a wrong predicate breaks, and the reason this is not a startup rule.**
    // A joiner started with `--raft-join` bootstraps EMPTY, so it never campaigns and
    // writes nothing -- which is indistinguishable from a machine that has never run at
    // all. Both may enrol, and a predicate that refused either would refuse the whole
    // population this mode exists for.
    auto const fresh = ReadConsensusHistory(scratch.Path());
    REQUIRE(fresh.has_value());
    CHECK(*fresh == ConsensusHistory::None);

    // And opening the storage again, as `ConsensusTier` would, still reports nothing:
    // the files existing is not the same fact as anything being recorded in them.
    {
        auto storage = Consensus::FileRaftStorage::Open(scratch.Path());
        REQUIRE(storage.has_value());
    }
    auto const opened = ReadConsensusHistory(scratch.Path());
    REQUIRE(opened.has_value());
    CHECK(*opened == ConsensusHistory::None);
}

TEST_CASE("A state directory that elected itself reports history", "[enrollment][trap]")
{
    Testing::ScratchDirectory scratch { "enroll-trap-elected" };
    StageSelfElected(scratch.Path(), "node-a");

    auto const trapped = ReadConsensusHistory(scratch.Path());
    REQUIRE(trapped.has_value());
    CHECK(*trapped == ConsensusHistory::Recorded);
}

TEST_CASE("A node whose state directory has run consensus is refused at enrol time, naming the wipe", "[enrollment][trap]")
{
    Testing::ScratchDirectory scratch { "enroll-trap-refusal" };
    StageSelfElected(scratch.Path(), "node-a");

    NodeConfig cfg;
    cfg.enrollFrom = "10.0.0.1:7000";
    cfg.clusterKeyFile = scratch / "cluster.key";
    cfg.clusterDir = scratch.Path();
    cfg.raftListen = "7100";
    cfg.raftSelf = "198.51.100.4";

    SystemRandomSource random;
    auto const refused = RunEnrollClient(cfg, ConfiguredCredential { cfg, nullptr }, random);
    REQUIRE(!refused.has_value());

    // **Assert what DISTINGUISHES.** Every refusal this mode can produce is a
    // `std::unexpected<std::string>`, so *it was refused* is true under half a dozen
    // unrelated faults -- a missing key file, an unreachable seed, no consensus
    // identity. What only THIS refusal says is the remedy and the reason the cheaper
    // remedy is wrong.
    CHECK(refused.error().contains("already holds consensus state"));
    CHECK(refused.error().contains(scratch.Path().string()));
    CHECK(refused.error().contains("delete"));

    // It names BOTH causes rather than asserting one. This directory holding a term and
    // a vote is a fact; *it bootstrapped a cluster of itself* is one of two readings of
    // it, and the other -- a node already in a cluster being pointed at another -- is
    // equally consistent with the same bytes. A confident wrong signal is worse than a
    // vague right one, and here they share a remedy so naming both costs nothing.
    CHECK(refused.error().contains("without --raft-join"));
    CHECK(refused.error().contains("already a member"));

    // And it says why clearing only the log is NOT the fix: that leaves this node's
    // minted identity in place, holding a vote record for the cluster it led. A wiped
    // state directory must get a new identity, which is what admission needs.
    CHECK(refused.error().contains("identity"));

    // Nothing was written. The refusal must not leave a half-enrolled machine behind,
    // and the key file is the observable half of that.
    CHECK(!std::filesystem::exists(cfg.clusterKeyFile));
}

TEST_CASE("The same state directory is fine at ordinary startup, which is why this is not a startup rule",
          "[enrollment][trap]")
{
    // **The case a suite skips, and the one that decides whether this ticket helps or
    // breaks every one-machine deployment.** A single node with `--listen-raft` and no
    // `--raft-join` bootstraps a cluster of itself ON PURPOSE, and its state directory
    // is byte-for-byte the shape the case above refuses. If this predicate had gone into
    // `StartupPolicyRejection`, that node would be refused at every boot.
    Testing::ScratchDirectory scratch { "enroll-trap-startup" };
    StageSelfElected(scratch.Path(), "node-a");

    NodeConfig cfg;
    cfg.clusterDir = scratch.Path();
    cfg.raftListen = "7100";
    cfg.raftSelf = "198.51.100.4";
    cfg.scheduler = "10.0.0.1:7000";
    cfg.nodeId = "node-a";

    // The startup table is what judges a serving configuration, and it is asked here
    // exactly as `main` asks it. It must have nothing to say about a self-elected state
    // directory, because the state directory is not a configuration and this table only
    // ever reads one.
    CHECK(!StartupPolicyRejection(cfg).has_value());

    // Stated as a pair rather than as one assertion, because the pair IS the property:
    // the same directory, refused on one path and accepted on the other. Either half
    // alone reads as a predicate that happens to answer correctly once.
    cfg.enrollFrom = "10.0.0.1:7000";
    cfg.clusterKeyFile = scratch / "cluster.key";
    SystemRandomSource random;
    CHECK(!RunEnrollClient(cfg, ConfiguredCredential { cfg, nullptr }, random).has_value());
}

// SPDX-License-Identifier: Apache-2.0
#include "EnrollClient.hpp"
#include "FormationEffects.hpp"
#include "RaftStoreArchiver.hpp"

#include <FastCache/Cluster/FleetEndpoints.hpp>
#include <FastCache/Cluster/FormationRecord.hpp>
#include <FastCache/Cluster/NodeMode.hpp>
#include <FastCache/Consensus/FileRaftStorage.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <core/platform/Clock.hpp>
#include <tests/FormationFakes.hpp>
#include <tests/NodeFormationControllerFakes.hpp>
#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/ScratchPath.hpp>
#include <tests/SecureRandomFakes.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using Cluster::NodeMode;
using Testing::LearnerIn;
using Testing::OfficeRosterWith;
using Testing::Pending;
using Testing::ScratchDirectory;
using Testing::Unwrap;

namespace
{
/// Who the laptop asks to be admitted as.
/// @return Its identity, under `TestKeyPair("n-laptop")`.
[[nodiscard]] JoinerIdentity Laptop()
{
    return JoinerIdentity { .nodeId = "n-laptop",
                            .nodeEndpoint = "laptop:6674",
                            .role = CompileCacheWire::EnrollRole::Learner,
                            .publicKey = Testing::TestKeyPair("n-laptop").PublicKey() };
}

/// Write a consensus store into @p directory whose every file holds @p tag.
/// @param directory The state directory.
/// @param tag What each file holds, so a case can tell two stores apart.
void WriteStore(std::filesystem::path const& directory, std::string_view tag)
{
    for (auto const name: Consensus::FileRaftStorage::StoreFileNames())
    {
        auto stream = std::ofstream { directory / name, std::ios::binary | std::ios::trunc };
        stream << tag;
    }
}

/// What every file of the store archived at @p at holds, when they all hold the same; else empty.
/// @param at An archive directory.
/// @return The one tag, or empty when a file is missing or two disagree.
[[nodiscard]] std::string StoreTagAt(std::filesystem::path const& at)
{
    auto tag = std::optional<std::string> {};
    for (auto const name: Consensus::FileRaftStorage::StoreFileNames())
    {
        auto stream = std::ifstream { at / name, std::ios::binary };
        if (!stream.is_open())
            return {};
        auto text = std::string {};
        stream >> text;
        if (tag.has_value() && *tag != text)
            return {};
        tag = std::move(text);
    }
    return tag.value_or(std::string {});
}

/// A record of a learner of `c-office` that was leaving its own `c-laptop` when it stopped.
/// @return The record, `archivePending` naming the solitary cluster.
[[nodiscard]] Cluster::FormationRecord InterruptedDissolve()
{
    auto interrupted = LearnerIn("c-laptop", "c-office");
    interrupted.archivePending = "c-laptop";
    return interrupted;
}
} // namespace

TEST_CASE("A crash after the learner record and before the archive resumes as an archive on the next start",
          "[node][formation][archive]")
{
    Testing::InMemoryFormationStore store;
    Testing::RecordingArchiver archiver { store };
    REQUIRE(store.Save(InterruptedDissolve()).has_value());

    auto const resumed = ResumeFormation(InterruptedDissolve(), store, archiver);
    REQUIRE(resumed.has_value());
    CHECK(archiver.Archived() == std::vector<std::string> { "c-laptop" });
    CHECK_FALSE(Unwrap(resumed).archivePending.has_value());
    CHECK(Unwrap(resumed).mode == NodeMode::Learner);
    auto const kept = store.Load();
    REQUIRE(kept.has_value());
    CHECK(Unwrap(Unwrap(kept)) == Unwrap(resumed));
}

TEST_CASE("A start with nothing pending resumes nothing", "[node][formation][archive]")
{
    Testing::InMemoryFormationStore store;
    Testing::RecordingArchiver archiver { store };
    auto const resumed = ResumeFormation(LearnerIn("c-laptop", "c-office"), store, archiver);
    REQUIRE(resumed.has_value());
    CHECK(Unwrap(resumed) == LearnerIn("c-laptop", "c-office"));
    CHECK(archiver.Archived().empty());
    CHECK(store.Saves().empty());
}

TEST_CASE("An archive that fails at start refuses the start and names what it could not move", "[node][formation][archive]")
{
    Testing::InMemoryFormationStore store;
    Testing::RecordingArchiver archiver { store };
    archiver.FailArchives("cannot move /var/lib/fastcache-node/raft-log: permission denied");

    auto const resumed = ResumeFormation(InterruptedDissolve(), store, archiver);
    REQUIRE_FALSE(resumed.has_value());
    CHECK(resumed.error().contains("raft-log"));
    CHECK(resumed.error().contains("c-laptop"));
    CHECK(store.Saves().empty()); // the record still says the archive is pending
}

TEST_CASE("Dissolving refuses a roster that does not name this node under its key, and writes nothing",
          "[node][formation][archive]")
{
    Testing::InMemoryFormationStore store;
    Testing::RecordingArchiver archiver { store };
    ScratchDirectory const scratch { "dissolve" };
    Cluster::FleetEndpointsFile endpoints { scratch.Path() };
    CapturingLogger logger;

    auto const dissolved = DissolveInto(Pending("c-laptop", 500, "c-office", "office:6674"),
                                        OfficeRosterWith("n-desk"),
                                        Laptop(),
                                        store,
                                        archiver,
                                        endpoints,
                                        logger);
    REQUIRE_FALSE(dissolved.has_value());
    CHECK(dissolved.error().contains("n-laptop"));
    CHECK(store.Saves().empty());
    CHECK(archiver.Archived().empty());
    CHECK(endpoints.Load().outcome == Cluster::FleetEndpointsLoad::Absent);
}

TEST_CASE("Dissolving refuses a roster no key of the proven fleet vouches for, and writes nothing",
          "[node][formation][archive]")
{
    Testing::InMemoryFormationStore store;
    Testing::RecordingArchiver archiver { store };
    ScratchDirectory const scratch { "dissolve-evil" };
    Cluster::FleetEndpointsFile endpoints { scratch.Path() };
    CapturingLogger logger;

    auto const dissolved = DissolveInto(Pending("c-laptop", 500, "c-office", "office:6674"),
                                        Testing::RosterWith("n-evil", "n-laptop"),
                                        Laptop(),
                                        store,
                                        archiver,
                                        endpoints,
                                        logger);
    REQUIRE_FALSE(dissolved.has_value());
    CHECK(dissolved.error().contains("key that proved"));
    CHECK(store.Saves().empty());
    CHECK(archiver.Archived().empty());
}

TEST_CASE("Dissolving refuses a fleet whose id could not name the archive of its store", "[node][formation][archive]")
{
    Testing::InMemoryFormationStore store;
    Testing::RecordingArchiver archiver { store };
    ScratchDirectory const scratch { "dissolve-id" };
    Cluster::FleetEndpointsFile endpoints { scratch.Path() };
    CapturingLogger logger;

    auto const dissolved = DissolveInto(Pending("c-laptop", 500, "../office", "office:6674"),
                                        OfficeRosterWith("n-laptop"),
                                        Laptop(),
                                        store,
                                        archiver,
                                        endpoints,
                                        logger);
    REQUIRE_FALSE(dissolved.has_value());
    CHECK(dissolved.error().contains("archive"));
    CHECK(store.Saves().empty());
    CHECK(archiver.Archived().empty());
}

TEST_CASE("Dissolving records the learner before the archive, and remembers the fleet's voters as seeds",
          "[node][formation][archive]")
{
    Testing::InMemoryFormationStore store;
    Testing::RecordingArchiver archiver { store };
    ScratchDirectory const scratch { "dissolve-seeds" };
    Cluster::FleetEndpointsFile endpoints { scratch.Path() };
    CapturingLogger logger;

    auto const dissolved = DissolveInto(Pending("c-laptop", 500, "c-office", "office:6674"),
                                        OfficeRosterWith("n-laptop"),
                                        Laptop(),
                                        store,
                                        archiver,
                                        endpoints,
                                        logger);
    REQUIRE(dissolved.has_value());
    CHECK(Unwrap(dissolved).mode == NodeMode::Learner);
    CHECK_FALSE(Unwrap(dissolved).joining.has_value());
    CHECK_FALSE(Unwrap(dissolved).archivePending.has_value());

    REQUIRE(store.Saves().size() == 2);
    CHECK(store.Saves()[0].archivePending == std::optional<std::string> { "c-laptop" });
    CHECK(archiver.ArchivedAfterSave() == 1); // the archive ran after the FIRST save
    CHECK(archiver.Archived() == std::vector<std::string> { "c-laptop" });

    auto loaded = Cluster::FleetEndpointsFile { scratch.Path() }.Load();
    REQUIRE(loaded.outcome == Cluster::FleetEndpointsLoad::Loaded);
    CHECK(loaded.endpoints.clusterId == "c-office");
    // The leader's node endpoint as the fleet stated it; the learner being admitted is no voter.
    CHECK(Cluster::RememberedSeeds(loaded.endpoints) == std::vector<std::string> { "office:6674" });
}

TEST_CASE("A cluster left twice keeps both of its stores, each in an archive of its own", "[node][formation][archive]")
{
    // Leave the fleet, rejoin it, leave it again: the second leave of the SAME cluster must neither
    // meet the first archive as a conflict -- which would strand the node -- nor write into it.
    ScratchDirectory const scratch { "archive-twice" };
    Testing::InMemoryFormationStore store;
    RaftStoreArchiver archiver { scratch.Path() };
    Cluster::FleetEndpointsFile endpoints { scratch.Path() };
    CapturingLogger logger;
    auto random = Testing::ScriptedSecureRandom { Testing::ScriptedSecureRandom::Ascending(2 * Cluster::ClusterIdBytes) };
    core::platform::ManualWallClock const wall { std::chrono::system_clock::time_point { std::chrono::seconds { 900 } } };

    WriteStore(scratch.Path(), "office-first");
    auto const firstLeave = ArchiveAndMint(LearnerIn("c-laptop", "c-office"), store, archiver, random, wall);
    REQUIRE(firstLeave.has_value());
    auto const minted = Unwrap(firstLeave).own.clusterId;

    // Its own new cluster ran, and then it asked the office again and was admitted.
    WriteStore(scratch.Path(), "own-second");
    auto rejoining = Unwrap(firstLeave);
    rejoining.mode = NodeMode::Pending;
    rejoining.joining = Cluster::JoinTarget { .summary = Testing::OfficeSummary("c-office", "office:6674"),
                                              .provenKey = Testing::TestKeyPair("n-office").PublicKey(),
                                              .askedAtUnixSeconds = 42 };
    auto const rejoined =
        DissolveInto(rejoining, OfficeRosterWith("n-laptop"), Laptop(), store, archiver, endpoints, logger);
    REQUIRE(rejoined.has_value());

    WriteStore(scratch.Path(), "office-second");
    auto const secondLeave = ArchiveAndMint(Unwrap(rejoined), store, archiver, random, wall);
    REQUIRE(secondLeave.has_value());

    auto const archive = scratch.Path() / ArchiveDirectoryName;
    CHECK(StoreTagAt(archive / "c-office") == "office-first");
    CHECK(StoreTagAt(archive / std::filesystem::path { minted }) == "own-second");
    CHECK(StoreTagAt(archive / "c-office.1") == "office-second");
    for (auto const name: Consensus::FileRaftStorage::StoreFileNames())
        CHECK_FALSE(std::filesystem::exists(scratch.Path() / name)); // the root holds no store it left
}

TEST_CASE("A leave a crash interrupted part-way is finished into its own archive, beside an earlier one",
          "[node][formation][archive]")
{
    // The second leave of c-office stopped after its first file moved: the record still says the archive
    // is pending, one file sits in the staging directory and the rest in the root. The resume finishes
    // THAT archive -- every file of the second store together, apart from the first leave's.
    ScratchDirectory const scratch { "archive-resume" };
    Testing::InMemoryFormationStore store;
    RaftStoreArchiver archiver { scratch.Path() };
    auto const archive = scratch.Path() / ArchiveDirectoryName;
    auto const names = Consensus::FileRaftStorage::StoreFileNames();
    std::filesystem::create_directories(archive / "c-office");
    WriteStore(archive / "c-office", "office-first");
    std::filesystem::create_directories(archive / "c-office.partial");
    WriteStore(scratch.Path(), "office-second");
    std::filesystem::rename(scratch.Path() / names[0], archive / "c-office.partial" / names[0]);

    auto interrupted = Testing::Minted("c-new", 900);
    interrupted.archivePending = "c-office";
    auto const resumed = ResumeFormation(interrupted, store, archiver);
    REQUIRE(resumed.has_value());
    CHECK(StoreTagAt(archive / "c-office") == "office-first");
    CHECK(StoreTagAt(archive / "c-office.1") == "office-second");
    CHECK_FALSE(std::filesystem::exists(archive / "c-office.partial"));
}

TEST_CASE("A forgotten node mints a cluster id it never had, and keeps nothing of the fleet", "[node][formation][archive]")
{
    Testing::InMemoryFormationStore store;
    Testing::RecordingArchiver archiver { store };
    auto random = Testing::ScriptedSecureRandom { std::vector<std::byte>(Cluster::ClusterIdBytes, std::byte { 0xcd }) };
    core::platform::ManualWallClock const wall { std::chrono::system_clock::time_point { std::chrono::seconds { 900 } } };
    auto forgotten = LearnerIn("c-laptop", "c-office");
    forgotten.askedJoins = { Cluster::AskedJoin { .clusterId = "c-office", .provenKey = {}, .askedAtUnixSeconds = 1 },
                             Cluster::AskedJoin { .clusterId = "c-lab", .provenKey = {}, .askedAtUnixSeconds = 2 } };

    auto const minted = ArchiveAndMint(forgotten, store, archiver, random, wall);
    REQUIRE(minted.has_value());
    CHECK(Unwrap(minted).mode == NodeMode::Solitary);
    CHECK(Unwrap(minted).own.clusterId == "cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd");
    CHECK(Unwrap(minted).own.createdAtUnixSeconds == 900);
    CHECK_FALSE(Unwrap(minted).fleet.has_value());
    CHECK_FALSE(Unwrap(minted).archivePending.has_value());
    CHECK(archiver.Archived() == std::vector<std::string> { "c-office" });
    CHECK(archiver.ArchivedAfterSave() == 1); // the new record was written before the store moved
    // A forget outranks an observation: the forgetting fleet's memo goes, every other one stays.
    REQUIRE(Unwrap(minted).askedJoins.size() == 1);
    CHECK(Unwrap(minted).askedJoins[0].clusterId == "c-lab");
}

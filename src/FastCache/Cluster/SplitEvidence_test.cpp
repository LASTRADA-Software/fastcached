// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/SplitEvidence.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <tests/FormationFakes.hpp>
#include <tests/RaftPeerKeyFakes.hpp>

using namespace FastCache;
using namespace FastCache::Cluster;
using CompileCacheWire::FleetState;
using CompileCacheWire::FleetSummary;
using Testing::TestKeyPair;

namespace
{
/// A member of this fleet recorded under @p id, holding @p key.
/// @param id Its id.
/// @param key Its recorded key, which every member carries.
/// @param seat Its seat.
/// @return The member record.
[[nodiscard]] ClusterMember Member(std::string const& id,
                                   Ed25519PublicKey key,
                                   MemberSeat seat = MemberSeat::Voter)
{
    return ClusterMember { .id = id,
                           .raftEndpoint = seat == MemberSeat::Voter ? id + ":6680" : std::string {},
                           .schedulerEndpoint = {},
                           .schedulerEndpointHistory = SchedulerEndpointHistory::NeverAnnounced,
                           .seat = seat,
                           .publicKey = key };
}

/// The office's applied state: its leader `n-office` and a learner `n-laptop`, each under its own key.
/// @return The state.
[[nodiscard]] ClusterState OfficeState()
{
    auto state = ClusterState {};
    state.members.push_back(Member("n-office", TestKeyPair("n-office").PublicKey()));
    state.members.push_back(Member("n-laptop", TestKeyPair("n-laptop").PublicKey(), MemberSeat::Learner));
    return state;
}

/// The lab's summary, spoken by @p speaker, listing @p members, proven by @p signer's key.
/// @param clusterId Which fleet it claims to be.
/// @param speaker Who it says is speaking.
/// @param members Whom it claims to record.
/// @param signer Whose key signs it.
/// @return The proven summary.
[[nodiscard]] ProvenFleetSummary Lab(std::string const& clusterId,
                                     std::string const& speaker,
                                     std::vector<std::string> members,
                                     std::string const& signer)
{
    auto summary = FleetSummary {
        .clusterId = clusterId, .state = FleetState::Established, .createdAtUnixSeconds = 1'700'000'000, .nodeId = speaker
    };
    summary.memberTotal = members.size();
    summary.members = std::move(members);
    return Testing::ProvenBy(summary, signer);
}

/// A record whose node asked @p clusterId's fleet under @p signer's key.
/// @param clusterId The fleet it asked.
/// @param signer Whose key proved that fleet when it asked.
/// @return The record.
[[nodiscard]] FormationRecord AskedUnder(std::string const& clusterId, std::string const& signer)
{
    auto record = FormationRecord {};
    record.mode = NodeMode::Voter;
    record.own = OwnCluster { .clusterId = "c-office", .createdAtUnixSeconds = 1'790'000'000 };
    record.askedJoins.push_back(
        AskedJoin { .clusterId = clusterId, .provenKey = TestKeyPair(signer).PublicKey(), .askedAtUnixSeconds = 5 });
    return record;
}
} // namespace

TEST_CASE("A fleet whose speaker is recorded here under the key it proved is this fleet split in two",
          "[cluster][formation][split]")
{
    auto const state = OfficeState();
    auto const record = FormationRecord {};

    // (A): the lab's speaker is n-laptop, which the office recorded -- a one-sided admission seen from
    // the side that recorded it. The laptop is a LEARNER here, so the reading says so, and names it.
    auto const byLearner = ReadSplit(state, "n-office", record, {}, Lab("c-lab", "n-laptop", { "n-lab" }, "n-laptop"));
    CHECK(byLearner.evidence == SplitEvidence::TheirSpeakerIsOurLearner);
    CHECK(byLearner.witness == "n-laptop");

    // ... and a VOTER of the office speaking for the lab is the kind that heals by itself.
    auto const byVoter = ReadSplit(state, "n-laptop", record, {}, Lab("c-lab", "n-office", { "n-lab" }, "n-office"));
    CHECK(byVoter.evidence == SplitEvidence::TheirSpeakerIsOurVoter);
    CHECK(byVoter.witness == "n-office");

    // A speaker the office does not record is not our member, whatever it lists.
    CHECK(SplitEvidenceFor(state, "n-office", record, {}, Lab("c-lab", "n-lab", { "n-lab" }, "n-lab"))
          == SplitEvidence::None);

    // A summary naming no speaker names nobody to look up.
    CHECK(SplitEvidenceFor(state, "n-office", record, {}, Lab("c-lab", "", { "n-laptop" }, "n-laptop"))
          == SplitEvidence::None);

    // A key this fleet REVOKED is never evidence, even under a record that still names it.
    auto revoked = state;
    revoked.revokedKeys.push_back(RevokedKey { .id = "n-laptop", .publicKey = TestKeyPair("n-laptop").PublicKey() });
    CHECK(SplitEvidenceFor(revoked, "n-office", record, {}, Lab("c-lab", "n-laptop", { "n-lab" }, "n-laptop"))
          == SplitEvidence::None);
}

TEST_CASE("A fleet a machine here asked, proven by the key it asked under and listing that machine, is this fleet split",
          "[cluster][formation][split]")
{
    auto const state = OfficeState();

    // (C) from this node's own memo: it asked c-lab under n-lab's key, and the lab lists it.
    auto const asked = AskedUnder("c-lab", "n-lab");
    auto const own = ReadSplit(state, "n-office", asked, {}, Lab("c-lab", "n-lab", { "n-lab", "n-office" }, "n-lab"));
    CHECK(own.evidence == SplitEvidence::WeAskedAndTheyListUs);
    CHECK(own.witness == "n-office"); // the asker, which is what an operator asks

    // A list that does not name the asker is no evidence: the lab never recorded it.
    CHECK(SplitEvidenceFor(state, "n-office", asked, {}, Lab("c-lab", "n-lab", { "n-lab" }, "n-lab"))
          == SplitEvidence::None);

    // A memo of another fleet says nothing about this one.
    CHECK(SplitEvidenceFor(state,
                           "n-office",
                           AskedUnder("c-elsewhere", "n-lab"),
                           {},
                           Lab("c-lab", "n-lab", { "n-lab", "n-office" }, "n-lab"))
          == SplitEvidence::None);

    // (C) from a memo a MEMBER announced, so the evidence does not depend on which machine leads.
    auto const none = FormationRecord {};
    auto const byLaptop = std::array { AskedJoinBy {
        .askerId = "n-laptop", .clusterId = "c-lab", .provenKey = TestKeyPair("n-lab").PublicKey() } };
    auto const announcedRead =
        ReadSplit(state, "n-office", none, byLaptop, Lab("c-lab", "n-lab", { "n-lab", "n-laptop" }, "n-lab"));
    CHECK(announcedRead.evidence == SplitEvidence::WeAskedAndTheyListUs);
    CHECK(announcedRead.witness == "n-laptop");

    // ... but only while that member is still recorded here: a machine this fleet no longer records
    // speaks for nobody in it.
    auto const byStranger = std::array { AskedJoinBy {
        .askerId = "n-gone", .clusterId = "c-lab", .provenKey = TestKeyPair("n-lab").PublicKey() } };
    CHECK(SplitEvidenceFor(state, "n-office", none, byStranger, Lab("c-lab", "n-lab", { "n-lab", "n-gone" }, "n-lab"))
          == SplitEvidence::None);
}

TEST_CASE("An older fleet listing this fleet's leader, proven by a key nobody here holds for it, is not a split",
          "[cluster][formation][split][security]")
{
    // The one-beacon takeover the evidence rule exists to refuse. Ids are public -- they ride every
    // beacon -- so an attacker mints a fleet older than the office, speaks AS the office's leader,
    // claims the id of a fleet the office once asked, and lists every machine the office has. Every
    // claim is its own; the key it proves with is none this fleet holds for any of them.
    auto const state = OfficeState();
    auto const asked = AskedUnder("c-lab", "n-lab");
    auto summary = FleetSummary { .clusterId = "c-lab",
                                  .state = FleetState::Established,
                                  .createdAtUnixSeconds = 1,
                                  .nodeId = "n-office",
                                  .members = { "n-office", "n-laptop" },
                                  .memberTotal = 2 };
    auto const evil = Testing::ProvenBy(summary, "n-evil");
    auto const announced = std::array { AskedJoinBy {
        .askerId = "n-laptop", .clusterId = "c-lab", .provenKey = TestKeyPair("n-lab").PublicKey() } };
    CHECK(SplitEvidenceFor(state, "n-office", asked, announced, evil) == SplitEvidence::None);

    // So the office stays foreign to it, however old it claims to be.
    auto const office = FleetSummary { .clusterId = "c-office",
                                       .state = FleetState::Established,
                                       .createdAtUnixSeconds = 1'790'000'000,
                                       .nodeId = "n-office" };
    CHECK(ClassifyEncounter(office, evil, SplitEvidenceFor(state, "n-office", asked, announced, evil))
          == Encounter::ForeignFleet);
}

TEST_CASE("A summary lists first the members seen speaking for another fleet, then voters, then learners",
          "[cluster][formation][split]")
{
    // The order is what a datagram's cut keeps: the machines another fleet's evidence (C) needs are
    // the last to be cut.
    auto state = ClusterState {};
    state.members.push_back(Member("n-c", TestKeyPair("n-c").PublicKey()));
    state.members.push_back(Member("n-a", TestKeyPair("n-a").PublicKey()));
    state.members.push_back(Member("n-l2", TestKeyPair("n-l2").PublicKey(), MemberSeat::Learner));
    state.members.push_back(Member("n-l1", TestKeyPair("n-l1").PublicKey(), MemberSeat::Learner));
    state.members.push_back(Member("n-b", TestKeyPair("n-b").PublicKey()));

    // Nobody seen elsewhere: voters by id, then learners by id.
    CHECK(SummaryMembers(state, {}) == std::vector<std::string> { "n-a", "n-b", "n-c", "n-l1", "n-l2" });

    // Seen elsewhere, newest first, each once, and only while recorded here.
    auto const spoke = std::vector<std::string> { "n-l2", "n-gone", "n-c", "n-l2" };
    CHECK(SummaryMembers(state, spoke) == std::vector<std::string> { "n-l2", "n-c", "n-a", "n-b", "n-l1" });
}

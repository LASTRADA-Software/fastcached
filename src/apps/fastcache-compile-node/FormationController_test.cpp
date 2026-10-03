// SPDX-License-Identifier: Apache-2.0
#include "EnrollClient.hpp"
#include "FormationController.hpp"

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Cluster/FleetEndpoints.hpp>
#include <FastCache/Cluster/FormationRecord.hpp>
#include <FastCache/Cluster/NodeMode.hpp>
#include <FastCache/Cluster/ProvenFleet.hpp>
#include <FastCache/Cluster/Roster.hpp>
#include <FastCache/Cluster/SeedSources.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <future>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <core/platform/Clock.hpp>
#include <tests/BoundedWait.hpp>
#include <tests/FormationFakes.hpp>
#include <tests/NodeFormationControllerFakes.hpp>
#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/ScratchPath.hpp>
#include <tests/SecureRandomFakes.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using Cluster::ClusterMember;
using Cluster::ClusterState;
using Cluster::FormationRecord;
using Cluster::MemberSeat;
using Cluster::NodeMode;
using Cluster::RevokedKey;
using CompileCacheWire::FleetState;
using CompileCacheWire::FleetSummary;
using Testing::LearnerIn;
using Testing::Minted;
using Testing::OfficeCreatedAt;
using Testing::OfficeRosterWith;
using Testing::OfficeSummary;
using Testing::Pending;
using Testing::Unwrap;

namespace
{
/// What the wall clock reads when a machine starts: a believable instant, so a record's times are not
/// all zero and a zero cannot pass for one this controller wrote.
constexpr std::chrono::seconds MachineStartsAt { 1'700'000'000 };

/// Fleet @p clusterId as a beacon proves it: in @p state, created at @p created, its leader
/// answering at @p endpoint.
/// @param clusterId The fleet's id.
/// @param state Its state.
/// @param created When it was created.
/// @param endpoint Where its leader answers the `0xFC` port.
/// @return The proven fleet, from a beacon.
[[nodiscard]] Cluster::ProvenFleet Proven(std::string_view clusterId,
                                          FleetState state,
                                          std::uint64_t created,
                                          std::string_view endpoint)
{
    return Testing::ProvenBeacon(FleetSummary { .clusterId = std::string { clusterId },
                                                .state = state,
                                                .createdAtUnixSeconds = created,
                                                .leaderId = "",
                                                .leaderNodeEndpoint = std::string { endpoint },
                                                .nodeId = "",
                                                .raftEndpoint = "" });
}

/// One reading, as the channel hands it on.
/// @param progress What the reading says.
/// @param detail Its detail.
/// @param roster The roster, for an admission.
/// @return The reading.
[[nodiscard]] EnrollReading Reading(EnrollProgress progress, std::string detail, std::vector<std::byte> roster = {})
{
    return EnrollReading {
        .progress = progress, .detail = std::move(detail), .roster = std::move(roster), .certificate = {}
    };
}

/// @param roster The roster the fleet handed over.
/// @return An admission carrying it.
[[nodiscard]] EnrollReading Admitted(std::vector<std::byte> roster)
{
    return Reading(EnrollProgress::Admitted, "admitted", std::move(roster));
}

/// @return A person's refusal.
[[nodiscard]] EnrollReading Refused()
{
    return Reading(EnrollProgress::Refused, "an operator rejected it");
}

/// @param endpoint Where the answering node says the leader is.
/// @return A `NotLeader` redirect to it.
[[nodiscard]] EnrollReading Redirect(std::string endpoint)
{
    return Reading(EnrollProgress::Redirect, std::move(endpoint));
}

/// @return A fleet that has not decided yet.
[[nodiscard]] EnrollReading Waiting()
{
    return Reading(EnrollProgress::Waiting, "waiting for an operator");
}

/// @param endpoint The endpoint that could not be reached.
/// @return What a poll of it reads as.
[[nodiscard]] EnrollReading Unreachable(std::string_view endpoint)
{
    return Reading(EnrollProgress::Fatal, std::format("cannot reach {}", endpoint));
}

/// A member record, every field stated.
/// @param id Its id.
/// @param raftEndpoint Where its Raft port answers.
/// @param seat Its seat.
/// @param publicKey The key recorded for it, when one is.
/// @return The member.
[[nodiscard]] ClusterMember Member(std::string id,
                                   std::string raftEndpoint,
                                   MemberSeat seat,
                                   std::optional<Ed25519PublicKey> publicKey = std::nullopt)
{
    return ClusterMember { .id = std::move(id),
                           .raftEndpoint = std::move(raftEndpoint),
                           .schedulerEndpoint = {},
                           .schedulerEndpointHistory = Cluster::SchedulerEndpointHistory::NeverAnnounced,
                           .seat = seat,
                           .publicKey = publicKey };
}

/// Whether any line @p logger captured contains @p text.
/// @param logger The logger.
/// @param text What to look for.
/// @return True when a line contains it.
[[nodiscard]] bool Logged(CapturingLogger const& logger, std::string_view text)
{
    return std::ranges::any_of(logger.Snapshot(),
                               [text](CapturingLogger::Record const& record) { return record.message.contains(text); });
}

/// Releases held archives when a case leaves, however it leaves: a failed REQUIRE unwinds past any
/// explicit release, and a held archive then blocks the beat a `std::jthread` joins.
struct ReleaseOnExit
{
    /// @param held The archiver to release.
    explicit ReleaseOnExit(Testing::RecordingArchiver& held) noexcept:
        archiver { held }
    {
    }

    ReleaseOnExit(ReleaseOnExit const&) = delete;
    ReleaseOnExit& operator=(ReleaseOnExit const&) = delete;
    ReleaseOnExit(ReleaseOnExit&&) = delete;
    ReleaseOnExit& operator=(ReleaseOnExit&&) = delete;

    ~ReleaseOnExit()
    {
        archiver.ReleaseArchives();
    }

    Testing::RecordingArchiver& archiver; ///< What it releases.
};

/// One machine: a controller over one of each fake, built from the record it starts with.
struct Machine
{
    /// @param nodeId Its id; its key is `TestKeyPair(nodeId)`.
    /// @param record The record it starts with.
    /// @param typedSeeds The `--fleet-seed` values it was given, if any.
    Machine(std::string const& nodeId, FormationRecord record, std::vector<std::string> typedSeeds = {}):
        typed { std::move(typedSeeds) },
        self { .nodeId = nodeId,
               .publicKey = Testing::TestKeyPair(nodeId).PublicKey(),
               .nodeEndpoint = nodeId.substr(2) + ":6674",
               .raftEndpoint = nodeId.substr(2) + ":6680" },
        controller { FormationParts { .store = store,
                                      .archiver = archiver,
                                      .enroll = enroll,
                                      .probe = probe,
                                      .endpoints = endpoints,
                                      .seeds = [this] { return Seeds(); },
                                      .reform = reform,
                                      .clock = clock,
                                      .wall = wall,
                                      .random = random,
                                      .metrics = metrics,
                                      .logger = logger },
                     self,
                     std::move(record) }
    {
    }

    /// @return The typed seeds, as `OrderSeeds` hands them on.
    [[nodiscard]] std::vector<Cluster::SeedCandidate> Seeds() const
    {
        auto seeds = std::vector<Cluster::SeedCandidate> {};
        for (auto const& endpoint: typed)
            seeds.push_back(Cluster::SeedCandidate { .endpoint = endpoint, .source = Cluster::SeedSource::FleetSeedFlag });
        return seeds;
    }

    Testing::InMemoryFormationStore store;
    Testing::RecordingArchiver archiver { store };
    Testing::ScriptedEnrollChannel enroll;
    Testing::ScriptedFleetProbe probe;
    Testing::ScratchDirectory scratch { "formation-controller" };
    Cluster::FleetEndpointsFile endpoints { scratch.Path() };
    Testing::CountingReformSignal reform;
    core::platform::ManualClock clock;
    core::platform::ManualWallClock wall { std::chrono::system_clock::time_point { MachineStartsAt } };
    Testing::ScriptedSecureRandom random { Testing::ScriptedSecureRandom::Ascending(64) };
    AtomicMetricsSink metrics;
    CapturingLogger logger;
    std::vector<std::string> typed;
    SelfFacts self;
    FormationController controller;
};

/// The wall clock's reading when a machine starts, in whole seconds, as a record stores it.
constexpr auto StartSeconds = static_cast<std::uint64_t>(MachineStartsAt.count());
} // namespace

TEST_CASE("A solitary node that proves an older solitary cluster records the join before it asks",
          "[node][formation][controller]")
{
    Machine laptop { "n-laptop", Minted("c-laptop", 500) };
    laptop.controller.OnFleetProven(Proven("c-office", FleetState::Solitary, OfficeCreatedAt, "office:6674"));
    laptop.controller.Tick();

    REQUIRE(laptop.store.Saves().size() == 1);
    auto const& written = laptop.store.Saves()[0];
    CHECK(written.mode == NodeMode::Pending);
    REQUIRE(written.joining.has_value());
    CHECK(Unwrap(written.joining).summary.clusterId == "c-office");
    CHECK(Unwrap(written.joining).provenKey == Testing::TestKeyPair("c-office").PublicKey());
    CHECK(Unwrap(written.joining).askedAtUnixSeconds == StartSeconds);
    // The memo split healing reads later is written in the same record as the ask.
    REQUIRE(written.askedJoins.size() == 1);
    CHECK(written.askedJoins[0].clusterId == "c-office");
    CHECK(laptop.enroll.Asked().empty()); // written first; the first poll is the NEXT beat
    CHECK(laptop.reform.Requests() == 0);
    CHECK(laptop.controller.Current().state == FleetState::Solitary); // pending keeps announcing solitary
    CHECK(laptop.controller.Current().clusterId == "c-laptop");       // and keeps running its own cluster
    CHECK(laptop.metrics.Read(IMetricsSink::Counter::FormationYields) == 1);

    laptop.controller.Tick();
    REQUIRE(laptop.enroll.Asked().size() == 1);
    CHECK(laptop.enroll.Asked()[0].endpoint == "office:6674");
    CHECK(laptop.enroll.Asked()[0].self.role == CompileCacheWire::EnrollRole::Learner);
    CHECK(laptop.enroll.Asked()[0].self.nodeId == "n-laptop");
    CHECK(laptop.enroll.Asked()[0].self.publicKey == laptop.self.publicKey);
    CHECK(laptop.enroll.Asked()[0].self.nodeEndpoint == laptop.self.nodeEndpoint);
}

TEST_CASE("A younger solitary node is asked and does not ask", "[node][formation][controller]")
{
    Machine office { "n-office", Minted("c-office", OfficeCreatedAt) };
    office.controller.OnFleetProven(Proven("c-laptop", FleetState::Solitary, 500, "laptop:6674"));
    office.controller.Tick();
    CHECK(office.store.Saves().empty());
    CHECK(office.controller.Mode() == NodeMode::Solitary);
    CHECK(office.metrics.Read(IMetricsSink::Counter::FormationYields) == 0);
}

TEST_CASE("A fleet whose id could not name an archive is never asked", "[node][formation][controller][archive]")
{
    // Its id would one day name the directory this node keeps the fleet's store in; the dissolve
    // refuses such a fleet, so asking it would start a join that cannot finish.
    Machine laptop { "n-laptop", Minted("c-laptop", 500) };
    laptop.controller.OnFleetProven(Proven("c.office", FleetState::Established, OfficeCreatedAt, "office:6674"));
    laptop.controller.Tick();
    CHECK(laptop.controller.Mode() == NodeMode::Solitary);
    CHECK(laptop.store.Saves().empty());

    // Never silently: named, with the rule it fails -- and once, however often it is proven again.
    laptop.controller.OnFleetProven(Proven("c.office", FleetState::Established, OfficeCreatedAt, "office:6674"));
    laptop.controller.Tick();
    auto const named = std::ranges::count_if(laptop.logger.Snapshot(), [](CapturingLogger::Record const& record) {
        return record.message.contains("'c.office'") && record.message.contains("archive");
    });
    CHECK(named == 1);

    // The control: the same fleet under an id that can.
    laptop.controller.OnFleetProven(Proven("c-office", FleetState::Established, OfficeCreatedAt, "office:6674"));
    laptop.controller.Tick();
    CHECK(laptop.controller.Mode() == NodeMode::Pending);
}

TEST_CASE("A node with a typed seed asks it before it decides on a beacon fleet", "[node][formation][controller]")
{
    Machine laptop { "n-laptop", Minted("c-laptop", 500), { "office:6674" } };
    laptop.probe.Answer("office:6674",
                        Testing::ProvenSeed(OfficeSummary("c-office", "office:6674"), Cluster::SeedSource::FleetSeedFlag));
    laptop.controller.OnFleetProven(Proven("c-rogue", FleetState::Established, 1, "rogue:6674")); // a beacon, older

    laptop.controller.Tick(); // asks the seed; decides nothing yet
    REQUIRE(laptop.probe.Asked().size() == 1);
    CHECK(laptop.probe.Asked()[0].endpoint == "office:6674");
    CHECK(laptop.controller.Mode() == NodeMode::Solitary);
    CHECK(laptop.store.Saves().empty());

    laptop.controller.Tick();
    REQUIRE(laptop.controller.Mode() == NodeMode::Pending);
    auto const record = laptop.controller.Record();
    REQUIRE(record.joining.has_value());
    CHECK(Unwrap(record.joining).summary.clusterId == "c-office");
    CHECK(laptop.probe.Asked().size() == 1); // a typed seed is asked ONCE before the decision, not every beat
}

TEST_CASE("A learner seeing an older established fleet stays and never yields to a foreign fleet",
          "[node][formation][controller]")
{
    Machine laptop { "n-laptop", LearnerIn("c-laptop", "c-office") };
    laptop.controller.OnFleetProven(Proven("c-elsewhere", FleetState::Established, 1, "elsewhere:6674"));
    laptop.controller.Tick();
    CHECK(laptop.controller.Mode() == NodeMode::Learner);
    CHECK(laptop.store.Saves().empty());
    CHECK(laptop.enroll.Asked().empty());
    CHECK(laptop.controller.Current().clusterId == "c-office");
    CHECK(laptop.controller.Current().createdAtUnixSeconds == OfficeCreatedAt); // the fleet's age, never zero
    CHECK(laptop.controller.Current().raftEndpoint.empty());                    // a learner's listener is closed
}

TEST_CASE("Approval writes the learner record before the store is archived and then asks for a reform",
          "[node][formation][controller]")
{
    Machine laptop { "n-laptop", Pending("c-laptop", 500, "c-office", "office:6674") };
    laptop.enroll.Script({ Admitted(OfficeRosterWith("n-laptop")) });
    laptop.controller.Tick();

    REQUIRE(laptop.store.Saves().size() == 2);
    auto const& learner = laptop.store.Saves()[0];
    CHECK(learner.mode == NodeMode::Learner);
    CHECK(learner.archivePending == std::optional<std::string> { "c-laptop" });
    CHECK_FALSE(learner.joining.has_value());
    REQUIRE(learner.fleet.has_value());
    CHECK(Unwrap(learner.fleet).clusterId == "c-office");
    CHECK(Unwrap(learner.fleet).roster == OfficeRosterWith("n-laptop"));
    CHECK(Unwrap(learner.fleet).createdAtUnixSeconds == OfficeCreatedAt); // what the fleet said when it was asked
    CHECK(learner.askedJoins.size() == 1);                                // the memo outlives the approval
    CHECK(laptop.archiver.Archived() == std::vector<std::string> { "c-laptop" });
    CHECK(laptop.archiver.ArchivedAfterSave() == 1); // the archive ran after the FIRST save
    CHECK_FALSE(laptop.store.Saves()[1].archivePending.has_value());
    CHECK(laptop.reform.Requests() == 1);
}

TEST_CASE("An archive that fails after the learner record is written still moves the node, for the next start to finish",
          "[node][formation][controller][archive]")
{
    // The record says where the node is going BEFORE the first file moves, so a move stopped there
    // is not undone: the node IS a learner with its solitary store still to put away, and the reform
    // hands it to the next start, which finishes the archive before consensus opens the directory.
    Machine laptop { "n-laptop", Pending("c-laptop", 500, "c-office", "office:6674") };
    laptop.archiver.FailArchives("access denied");
    laptop.enroll.Script({ Admitted(OfficeRosterWith("n-laptop")) });
    laptop.controller.Tick();

    REQUIRE(laptop.store.Saves().size() == 1);
    CHECK(laptop.store.Saves()[0].archivePending == std::optional<std::string> { "c-laptop" });
    CHECK(laptop.controller.Mode() == NodeMode::Learner);
    CHECK(laptop.controller.Record() == laptop.store.Saves()[0]); // what the store holds, never a guess
    CHECK(laptop.reform.Requests() == 1);
    CHECK(Logged(laptop.logger, "access denied"));
}

TEST_CASE("An admission whose roster does not record this node under its key is not believed",
          "[node][formation][controller]")
{
    // The roster admits a DIFFERENT machine under that id's key: whoever answered is not the cluster
    // this node asked, and joining on it would be a confident wrong move.
    Machine laptop { "n-laptop", Pending("c-laptop", 500, "c-office", "office:6674") };
    laptop.enroll.Script({ Admitted(OfficeRosterWith("n-someone-else")) });
    laptop.controller.Tick();
    CHECK(laptop.controller.Mode() == NodeMode::Pending);
    CHECK(laptop.store.Saves().empty());
    CHECK(laptop.archiver.Archived().empty());
    CHECK(Logged(laptop.logger, "does not record n-laptop"));

    // The control: the roster that does record it is believed.
    laptop.enroll.Script({ Admitted(OfficeRosterWith("n-laptop")) });
    laptop.controller.Tick();
    CHECK(laptop.controller.Mode() == NodeMode::Learner);
}

TEST_CASE("An admission whose roster no key of the proven fleet vouches for is refused, and the node stays put",
          "[node][formation][controller]")
{
    // The join was decided on a summary n-office's key signed; the Enroll exchange after it is not
    // signed. A roster that records this node correctly but names none of the proven fleet's keys
    // came from whoever answered at that endpoint -- joining it would hand them the node's cluster.
    Machine laptop { "n-laptop", Pending("c-laptop", 500, "c-office", "office:6674") };
    laptop.enroll.Script({ Admitted(Testing::RosterWith("n-evil", "n-laptop")) });
    laptop.controller.Tick();

    CHECK(laptop.controller.Mode() == NodeMode::Pending);
    CHECK(laptop.controller.Record().own.clusterId == "c-laptop"); // its own cluster, untouched
    CHECK(laptop.store.Saves().empty());
    CHECK(laptop.archiver.Archived().empty());
    CHECK(laptop.reform.Requests() == 0);
    CHECK(laptop.metrics.Read(IMetricsSink::Counter::FormationAdmissionsRefused) == 1);
    CHECK(Logged(laptop.logger, "records no member under the key that proved the fleet c-office"));

    // The control: the office's own roster is believed.
    laptop.enroll.Script({ Admitted(OfficeRosterWith("n-laptop")) });
    laptop.controller.Tick();
    CHECK(laptop.controller.Mode() == NodeMode::Learner);
    CHECK(laptop.metrics.Read(IMetricsSink::Counter::FormationAdmissionsRefused) == 1);
}

TEST_CASE("A rejection returns the node to solitary and it does not ask that fleet again for an hour",
          "[node][formation][controller]")
{
    Machine laptop { "n-laptop", Pending("c-laptop", 500, "c-office", "office:6674") };
    laptop.enroll.Script({ Refused() });
    laptop.controller.Tick();
    CHECK(laptop.controller.Mode() == NodeMode::Solitary);
    auto const refused = laptop.controller.Record();
    REQUIRE(refused.rejectedBy.has_value());
    CHECK(Unwrap(refused.rejectedBy).clusterId == "c-office");
    CHECK(Unwrap(refused.rejectedBy).atUnixSeconds == StartSeconds);
    CHECK_FALSE(refused.joining.has_value());
    CHECK(refused.askedJoins.size() == 1); // the memo outlives the refusal
    CHECK(laptop.reform.Requests() == 0);

    laptop.controller.OnFleetProven(Proven("c-office", FleetState::Established, OfficeCreatedAt, "office:6674"));
    laptop.controller.Tick();
    CHECK(laptop.controller.Mode() == NodeMode::Solitary);

    laptop.wall.advance(RejectedRetryAfter + std::chrono::seconds { 1 });
    laptop.controller.OnFleetProven(Proven("c-office", FleetState::Established, OfficeCreatedAt, "office:6674"));
    laptop.controller.Tick();
    CHECK(laptop.controller.Mode() == NodeMode::Pending);
}

TEST_CASE("A fleet that stops answering is abandoned after ten minutes and the node stays solitary",
          "[node][formation][controller]")
{
    Machine laptop { "n-laptop", Pending("c-laptop", 500, "c-office", "office:6674") };
    laptop.enroll.ScriptForever(Unreachable("office:6674"));
    laptop.controller.Tick();
    laptop.clock.advance(PendingGiveUpAfter - std::chrono::seconds { 1 });
    laptop.controller.Tick();
    CHECK(laptop.controller.Mode() == NodeMode::Pending);
    CHECK(laptop.metrics.Read(IMetricsSink::Counter::FormationJoinsAbandoned) == 0);

    laptop.clock.advance(std::chrono::seconds { 2 });
    laptop.controller.Tick();
    CHECK(laptop.controller.Mode() == NodeMode::Solitary);
    CHECK(laptop.metrics.Read(IMetricsSink::Counter::FormationJoinsAbandoned) == 1);
    CHECK(laptop.reform.Requests() == 0);

    // The join is cleared and its memo is KEPT: the evidence a split heals on outlives the ask.
    auto const record = laptop.controller.Record();
    CHECK_FALSE(record.joining.has_value());
    REQUIRE(record.askedJoins.size() == 1);
    CHECK(record.askedJoins[0].clusterId == "c-office");
}

TEST_CASE("A redirect is followed and a chain longer than the bound goes back to the target",
          "[node][formation][controller]")
{
    Machine laptop { "n-laptop", Pending("c-laptop", 500, "c-office", "office:6674") };
    laptop.enroll.Script({ Redirect("a:6674"), Redirect("b:6674"), Redirect("c:6674"), Redirect("d:6674"), Waiting() });
    for ([[maybe_unused]] auto const beat: std::views::iota(0, 5))
        laptop.controller.Tick();
    CHECK(laptop.enroll.AskedEndpoints()
          == std::vector<std::string> { "office:6674", "a:6674", "b:6674", "c:6674", "office:6674" });
    static_assert(MaxEnrollRedirects == 3, "the script above follows exactly MaxEnrollRedirects hops");
    CHECK(laptop.controller.Mode() == NodeMode::Pending);
}

TEST_CASE("The founder becomes a voter when its cluster records another machine", "[node][formation][controller]")
{
    Machine office { "n-office", Minted("c-office", OfficeCreatedAt) };
    auto state = ClusterState {};
    state.members.push_back(Member("n-office", "office:6680", MemberSeat::Voter));
    office.controller.OnClusterState(state, "c-office", "n-office", "office:6674");
    CHECK(office.controller.Mode() == NodeMode::Solitary);
    CHECK(office.store.Saves().empty());

    state.members.push_back(Member("n-laptop", "", MemberSeat::Learner));
    office.controller.OnClusterState(state, "c-office", "n-office", "office:6674");
    CHECK(office.controller.Mode() == NodeMode::Voter);
    CHECK(office.reform.Requests() == 0); // the same cluster, now a fleet: nothing is rebuilt
    auto const summary = office.controller.Current();
    CHECK(summary.state == FleetState::Established);
    CHECK(summary.clusterId == "c-office");
    CHECK(summary.leaderId == "n-office");
    CHECK(summary.leaderNodeEndpoint == "office:6674");
    CHECK(summary.raftEndpoint == "office:6680");
}

TEST_CASE("A learner seated as a voter reforms and a forgotten learner mints a new solitary cluster",
          "[node][formation][controller]")
{
    Machine laptop { "n-laptop", LearnerIn("c-laptop", "c-office") };
    auto state = ClusterState {};
    state.members.push_back(Member("n-office", "office:6680", MemberSeat::Voter));
    state.members.push_back(Member("n-laptop", "laptop:6680", MemberSeat::Voter, laptop.self.publicKey));
    laptop.controller.OnClusterState(state, "c-office", "n-office", "office:6674");
    CHECK(laptop.controller.Mode() == NodeMode::Voter);
    CHECK(laptop.reform.Requests() == 1);

    auto forgotten = ClusterState {};
    forgotten.members.push_back(Member("n-office", "office:6680", MemberSeat::Voter));
    forgotten.revokedKeys.push_back(RevokedKey { .id = "n-laptop", .publicKey = laptop.self.publicKey });
    laptop.controller.OnClusterState(forgotten, "c-office", "n-office", "office:6674");
    CHECK(laptop.controller.Mode() == NodeMode::Solitary);
    CHECK(laptop.archiver.Archived() == std::vector<std::string> { "c-office" });
    auto const record = laptop.controller.Record();
    CHECK(record.own.clusterId != "c-laptop"); // a NEW cluster, never the old one back
    CHECK(record.own.clusterId != "c-office");
    CHECK(record.own.createdAtUnixSeconds == StartSeconds);
    CHECK_FALSE(record.fleet.has_value());
    CHECK_FALSE(record.archivePending.has_value());
    CHECK(laptop.reform.Requests() == 2);
}

TEST_CASE("A learner opens a scheduler only once the state it applied seats it as a voter under its own key",
          "[node][formation][controller]")
{
    // A voter serves the scheduler its OWN worker proves itself to, and `NodeRoster::StandingOf`
    // answers that proof from the applied state by this id's recorded key. Were the mode to move on
    // anything less, the node's own worker would meet its own scheduler as `NotVoter` -- a two-voter
    // fleet whose second worker never registers.
    Machine laptop { "n-laptop", LearnerIn("c-laptop", "c-office") };
    auto const office = Member("n-office", "office:6680", MemberSeat::Voter, Testing::TestKeyPair("n-office").PublicKey());

    auto seatedAsLearner = ClusterState {};
    seatedAsLearner.members = { office, Member("n-laptop", "", MemberSeat::Learner, laptop.self.publicKey) };
    laptop.controller.OnClusterState(seatedAsLearner, "c-office", "n-office", "office:6674");
    CHECK(laptop.controller.Mode() == NodeMode::Learner);

    auto seatedWithoutKey = ClusterState {};
    seatedWithoutKey.members = { office, Member("n-laptop", "laptop:6680", MemberSeat::Voter) };
    laptop.controller.OnClusterState(seatedWithoutKey, "c-office", "n-office", "office:6674");
    CHECK(laptop.controller.Mode() == NodeMode::Learner);
    CHECK(Logged(laptop.logger, "without this machine's key"));

    auto seatedUnderAnotherKey = ClusterState {};
    seatedUnderAnotherKey.members = {
        office, Member("n-laptop", "laptop:6680", MemberSeat::Voter, Testing::TestKeyPair("n-impostor").PublicKey())
    };
    laptop.controller.OnClusterState(seatedUnderAnotherKey, "c-office", "n-office", "office:6674");
    CHECK(laptop.controller.Mode() == NodeMode::Learner);
    CHECK(laptop.store.Saves().empty());
    CHECK(laptop.reform.Requests() == 0);

    // The control: its own record, a voter's seat, its own key -- what `StandingOf` calls `Voter`.
    auto seated = ClusterState {};
    seated.members = { office, Member("n-laptop", "laptop:6680", MemberSeat::Voter, laptop.self.publicKey) };
    laptop.controller.OnClusterState(seated, "c-office", "n-office", "office:6674");
    CHECK(laptop.controller.Mode() == NodeMode::Voter);
    CHECK(laptop.reform.Requests() == 1);
}

TEST_CASE("A state of the cluster a node is leaving never seats it in the fleet it joined", "[node][formation][controller]")
{
    // Between the approval and the reform, the solitary cluster's consensus still runs -- and a
    // founder's own state records it as a voter under its own key. Read as the fleet's, that state
    // would seat the new learner as a voter of a fleet that never seated it.
    Machine laptop { "n-laptop", Pending("c-laptop", 500, "c-office", "office:6674") };
    laptop.enroll.Script({ Admitted(OfficeRosterWith("n-laptop")) });
    laptop.controller.Tick();
    REQUIRE(laptop.controller.Mode() == NodeMode::Learner);
    REQUIRE(laptop.reform.Requests() == 1);

    auto founders = ClusterState {};
    founders.members.push_back(Member("n-laptop", "laptop:6680", MemberSeat::Voter, laptop.self.publicKey));
    laptop.controller.OnClusterState(founders, "c-laptop", "n-laptop", "laptop:6674");
    CHECK(laptop.controller.Mode() == NodeMode::Learner);
    CHECK(laptop.reform.Requests() == 1);                // no second reform
    CHECK(laptop.controller.Current().leaderId.empty()); // nothing of that state was taken on

    // The control: the same seat, in the fleet's own state, does seat it.
    laptop.controller.OnClusterState(founders, "c-office", "n-office", "office:6674");
    CHECK(laptop.controller.Mode() == NodeMode::Voter);
    CHECK(laptop.reform.Requests() == 2);
}

TEST_CASE("What a node says about itself is answered while its archive waits on the disk",
          "[node][formation][controller][archive]")
{
    // Every beacon and proof reads `Current`, so a slow disk under a move must not stall discovery:
    // the move's disk work runs outside the lock `Current` takes, and what the store has already
    // kept is what `Current` says.
    Machine laptop { "n-laptop", Pending("c-laptop", 500, "c-office", "office:6674") };
    laptop.archiver.HoldArchives();
    laptop.enroll.Script({ Admitted(OfficeRosterWith("n-laptop")) });

    // Declared so they unwind in the order that frees everything: the release first, then the
    // answer it unblocks, then the beat it lets finish.
    std::jthread beat { [&laptop] { laptop.controller.Tick(); } };
    auto answer = std::future<FleetSummary> {};
    ReleaseOnExit const release { laptop.archiver };

    REQUIRE(Testing::WaitUntil(
        "the approval's archive to be entered",
        [&laptop] { return laptop.archiver.Entered(); },
        [] { return std::string { "the beat has not reached the archive" }; }));
    answer = std::async(std::launch::async, [&laptop] { return laptop.controller.Current(); });
    REQUIRE(Testing::WaitUntil(
        "Current() to answer while the archive is held",
        [&answer] { return answer.wait_for(std::chrono::seconds { 0 }) == std::future_status::ready; },
        [] { return std::string { "Current() is still waiting" }; }));

    // It answers with what the store has already kept: a learner of the office, its store still to
    // be put away.
    auto const said = answer.get();
    CHECK(said.clusterId == "c-office");
    CHECK(said.state == FleetState::Established);
}

TEST_CASE("An OwnKeyRevoked verdict counts only from a voter the roster holds", "[node][formation][controller]")
{
    Machine laptop { "n-laptop", LearnerIn("c-laptop", "c-office") }; // roster voters: n-office
    laptop.controller.OnOwnKeyRevoked("n-stranger");
    CHECK(laptop.controller.Mode() == NodeMode::Learner);
    CHECK(laptop.store.Saves().empty());
    laptop.controller.OnOwnKeyRevoked("n-laptop"); // a learner of the roster, and no voter
    CHECK(laptop.controller.Mode() == NodeMode::Learner);

    laptop.controller.OnOwnKeyRevoked("n-office");
    CHECK(laptop.controller.Mode() == NodeMode::Solitary);
    CHECK(laptop.archiver.Archived() == std::vector<std::string> { "c-office" });
}

TEST_CASE("A record that cannot be saved leaves the mode unchanged and says why", "[node][formation][controller]")
{
    Machine laptop { "n-laptop", Minted("c-laptop", 500) };
    laptop.store.FailSaves("disk full");
    laptop.controller.OnFleetProven(Proven("c-office", FleetState::Established, OfficeCreatedAt, "office:6674"));
    laptop.controller.Tick();
    CHECK(laptop.controller.Mode() == NodeMode::Solitary);
    CHECK_FALSE(laptop.controller.Record().joining.has_value());
    CHECK(Logged(laptop.logger, "disk full"));
    CHECK(laptop.metrics.Read(IMetricsSink::Counter::FormationYields) == 0); // nothing was decided
    CHECK(laptop.enroll.Asked().empty());
}

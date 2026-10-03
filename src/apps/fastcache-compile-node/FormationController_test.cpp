// SPDX-License-Identifier: Apache-2.0
#include "EnrollClient.hpp"
#include "EnrollmentWindow.hpp"
#include "FormationController.hpp"
#include "FormationRuntime.hpp"
#include "NodeFormation.hpp"

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Cluster/EnrollAdmissionSignature.hpp>
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
#include <tests/NodeConditionFakes.hpp>
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

/// An admission of whoever asked, signed over the nonce of the poll it answers by @p signer's key
/// for @p clusterId -- as `EnrollmentResponder` signs one.
/// @param signer Whose key signs it.
/// @param clusterId The cluster it admits into.
/// @param roster The roster the fleet handed over.
/// @return The scripted answer.
[[nodiscard]] Testing::ScriptedAnswer AdmittedBy(std::string signer, std::string clusterId, std::vector<std::byte> roster)
{
    return Testing::ScriptedAnswer { Reading(EnrollProgress::Admitted, "admitted", std::move(roster)),
                                     Testing::AdmissionSigning { .signer = std::move(signer),
                                                                 .clusterId = std::move(clusterId) } };
}

/// @param roster The roster the fleet handed over.
/// @return An admission as the office's leader answers it: signed by n-office, for c-office.
[[nodiscard]] Testing::ScriptedAnswer Admitted(std::vector<std::byte> roster)
{
    return AdmittedBy("n-office", "c-office", std::move(roster));
}

/// @param roster The roster.
/// @return An admission nobody signed.
[[nodiscard]] EnrollReading UnsignedAdmission(std::vector<std::byte> roster)
{
    return Reading(EnrollProgress::Admitted, "admitted", std::move(roster));
}

/// The reviewer's forged roster: n-evil its voter, the office's member under n-office's PUBLIC key
/// seated as @p seat, and n-laptop -- every fact in it one that rides every beacon.
/// @param seat Where n-office's key is seated.
/// @return The roster's bytes.
[[nodiscard]] std::vector<std::byte> CopiedRoster(MemberSeat seat)
{
    auto const member = [](std::string const& id, MemberSeat memberSeat) {
        return Cluster::RosterMember { .id = id,
                                       .raftEndpoint = memberSeat == MemberSeat::Voter ? id.substr(2) + ":6680" : "",
                                       .seat = memberSeat,
                                       .publicKey = Testing::TestKeyPair(id).PublicKey() };
    };
    auto roster = Cluster::Roster { .members = { member("n-evil", MemberSeat::Voter),
                                                 member("n-laptop", MemberSeat::Learner),
                                                 member("n-office", seat) },
                                    .principals = {},
                                    .revoked = {} };
    return Cluster::EncodeRoster(roster);
}

/// @return A person's refusal, nobody's signature on it.
[[nodiscard]] EnrollReading UnsignedRefusal()
{
    return Reading(EnrollProgress::Refused, "an operator rejected it");
}

/// @return A person's refusal, as the office's leader answers it: signed by n-office, for c-office.
[[nodiscard]] Testing::ScriptedAnswer Refused()
{
    return Testing::ScriptedAnswer { UnsignedRefusal(),
                                     Testing::AdmissionSigning { .signer = "n-office", .clusterId = "c-office" } };
}

/// @param endpoint Where the answering node says the leader is.
/// @return A `NotLeader` redirect to it.
[[nodiscard]] EnrollReading Redirect(std::string endpoint)
{
    return Reading(EnrollProgress::Redirect, std::move(endpoint));
}

/// @return A fleet that has not decided yet, nobody's signature on it.
[[nodiscard]] EnrollReading UnsignedWaiting()
{
    return Reading(EnrollProgress::Waiting, "waiting for an operator");
}

/// @return A fleet that has not decided yet, as the office's leader answers it.
[[nodiscard]] Testing::ScriptedAnswer Waiting()
{
    return Testing::ScriptedAnswer { UnsignedWaiting(),
                                     Testing::AdmissionSigning { .signer = "n-office", .clusterId = "c-office" } };
}

/// What c-office says, spoken by n-office, when @p leader leads it at `<host>:6674` under its own key:
/// what a joiner asks n-office after a redirect, to learn which key leads now.
/// @param leader The leader, `n-<host>`.
/// @return The summary, as n-office proves it.
[[nodiscard]] Cluster::ProvenFleetSummary OfficeNamingLeader(std::string const& leader)
{
    auto summary = Testing::OfficeSummary("c-office", std::format("{}:6674", leader.substr(2)));
    summary.leaderId = leader;
    summary.leaderKey = Testing::TestKeyPair(leader).PublicKey();
    return Testing::ProvenBy(summary, "n-office");
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
/// @param publicKey The key recorded for it; the id's own test key when none is named, since a
///        recorded member always holds one.
/// @return The member.
[[nodiscard]] ClusterMember Member(std::string id,
                                   std::string raftEndpoint,
                                   MemberSeat seat,
                                   std::optional<Ed25519PublicKey> publicKey = std::nullopt)
{
    auto const key = publicKey.value_or(Testing::TestKeyPair(id).PublicKey());
    return ClusterMember { .id = std::move(id),
                           .raftEndpoint = std::move(raftEndpoint),
                           .schedulerEndpoint = {},
                           .schedulerEndpointHistory = Cluster::SchedulerEndpointHistory::NeverAnnounced,
                           .seat = seat,
                           .publicKey = key };
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

/// Releases held saves when a case leaves, however it leaves: a failed REQUIRE unwinds past any
/// explicit release, and a held save then blocks the beat a `std::jthread` joins.
struct ReleaseOnExit
{
    /// @param held The store whose saves to release.
    explicit ReleaseOnExit(Testing::InMemoryFormationStore& held) noexcept:
        store { held }
    {
    }

    ReleaseOnExit(ReleaseOnExit const&) = delete;
    ReleaseOnExit& operator=(ReleaseOnExit const&) = delete;
    ReleaseOnExit(ReleaseOnExit&&) = delete;
    ReleaseOnExit& operator=(ReleaseOnExit&&) = delete;

    ~ReleaseOnExit()
    {
        store.ReleaseSaves();
    }

    Testing::InMemoryFormationStore& store; ///< What it releases.
};

/// One machine: a controller over one of each fake, built from the record it starts with.
struct Machine
{
    /// @param nodeId Its id; its key is `TestKeyPair(nodeId)`.
    /// @param record The record it starts with.
    /// @param typedSeeds The `--fleet-seed` values it was given, if any.
    /// @param nodeEndpoint The `0xFC` endpoint it advertises; by default one named after its id.
    /// @param reach Whether other machines can be its fleet at all.
    Machine(std::string const& nodeId,
            FormationRecord record,
            std::vector<std::string> typedSeeds = {},
            std::optional<std::string> const& nodeEndpoint = std::nullopt,
            FleetReachability reach = FleetReachability::Open):
        typed { std::move(typedSeeds) },
        self { .nodeId = nodeId,
               .publicKey = Testing::TestKeyPair(nodeId).PublicKey(),
               .nodeEndpoint = nodeEndpoint.value_or(nodeId.substr(2) + ":6674"),
               .raftEndpoint = nodeId.substr(2) + ":6680",
               .reach = reach },
        controller { FormationParts { .store = store,
                                      .enroll = enroll,
                                      .probe = probe,
                                      .endpoints = endpoints,
                                      .announced = announced,
                                      .admin = admin,
                                      .seeds = [this] { return Seeds(); },
                                      .reform = reform,
                                      .clock = clock,
                                      .wall = wall,
                                      .random = random,
                                      .metrics = metrics,
                                      .logger = logger,
                                      .judge = judge,
                                      .conditions = &conditions },
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
    Testing::ScriptedEnrollChannel enroll;
    Testing::ScriptedFleetProbe probe;
    Testing::ScratchDirectory scratch { "formation-controller" };
    Cluster::FleetEndpointsFile endpoints { scratch.Path() };
    Testing::ScriptedAnnouncedMemos announced;
    Testing::RecordingClusterAdmin admin;
    Testing::CountingReformSignal reform;
    core::platform::ManualClock clock;
    core::platform::ManualWallClock wall { std::chrono::system_clock::time_point { MachineStartsAt } };
    Testing::ScriptedSecureRandom random { Testing::ScriptedSecureRandom::Ascending(64) };
    AtomicMetricsSink metrics;
    CapturingLogger logger;
    Testing::ScriptedShapeJudge judge;
    NodeConditions conditions;
    std::vector<std::string> typed;
    SelfFacts self;
    FormationController controller;
};

/// A startup rule stated through the judge's own seam: it refuses a shape
/// by what the node's DERIVED schedulers are -- here, a learner registering with the office's node
/// port, which only the remembered fleet endpoints put there.
/// @param cfg A shaped configuration.
/// @return The refusal, or nothing.
[[nodiscard]] std::optional<std::string> RefuseRegisteringWithTheOffice(NodeConfig const& cfg)
{
    if (std::ranges::contains(SchedulersOf(cfg, AsConfigured), std::string { "office:6674" }))
        return std::string { "a test rule refuses registering with office:6674" };
    return std::nullopt;
}

/// The laptop's configuration as its pending record shapes it, for a production judge to copy.
/// @param pending The record.
/// @return The configuration.
[[nodiscard]] NodeConfig LaptopShapedBy(FormationRecord const& pending)
{
    auto cfg = NodeConfig {};
    cfg.nodeId = "n-laptop";
    cfg.identityPublicKey = Testing::TestKeyPair("n-laptop").PublicKey();
    cfg.hostNames = NodeHostNames { .fqdn = "laptop.corp.example", .dnsSuffix = {}, .withheld = {} };
    REQUIRE(ApplyFormation(cfg, pending, {}).has_value());
    return cfg;
}

/// The wall clock's reading when a machine starts, in whole seconds, as a record stores it.
constexpr auto StartSeconds = static_cast<std::uint64_t>(MachineStartsAt.count());
} // namespace

TEST_CASE("A node's summary lists the machines its cluster applied, and itself before it applied any",
          "[node][formation][controller][split]")
{
    // Every cluster starts as its founder alone, so a node that has applied no state yet lists itself.
    Machine office { "n-office", Minted("c-office", OfficeCreatedAt) };
    CHECK(office.controller.Current().members == std::vector<std::string> { "n-office" });
    CHECK(office.controller.Current().memberTotal == 1);

    auto state = ClusterState {};
    state.members.push_back(Member("n-office", "office:6680", MemberSeat::Voter));
    state.members.push_back(Member("n-laptop", "", MemberSeat::Learner, Testing::TestKeyPair("n-laptop").PublicKey()));
    state.members.push_back(Member("n-desk", "desk:6680", MemberSeat::Voter));
    office.controller.OnClusterState(state, "c-office", "n-office", "office:6674");
    CHECK(office.controller.Current().members == std::vector<std::string> { "n-desk", "n-office", "n-laptop" });
    CHECK(office.controller.Current().memberTotal == 3);

    // A member seen speaking for another proven fleet -- under the key this fleet records for it -- is
    // listed FIRST, so the other side's evidence is the last thing a datagram's cut drops.
    office.controller.OnFleetProven(Testing::ProvenBeacon(
        FleetSummary { .clusterId = "c-lab", .state = FleetState::Established, .nodeId = "n-laptop" }));
    CHECK(office.controller.Current().members.front() == "n-laptop");

    // A state of a cluster this node is not in never becomes its list.
    auto other = ClusterState {};
    other.members.push_back(Member("n-stranger", "stranger:6680", MemberSeat::Voter));
    office.controller.OnClusterState(other, "c-elsewhere", "n-stranger", "stranger:6674");
    CHECK(office.controller.Current().memberTotal == 3);
}

TEST_CASE("A member speaking for this node's own fleet is never listed as speaking for another",
          "[node][formation][controller][split]")
{
    // Discovery hands this node's own fleet on too, and the controller drops it by cluster id itself
    // rather than trusting the watch in front of it to have: listed first, a member speaking for its
    // OWN fleet would read, to the other side of a split, as evidence of one.
    Machine office { "n-office", Minted("c-office", OfficeCreatedAt) };
    auto state = ClusterState {};
    state.members.push_back(Member("n-office", "office:6680", MemberSeat::Voter));
    state.members.push_back(Member("n-laptop", "", MemberSeat::Learner, Testing::TestKeyPair("n-laptop").PublicKey()));
    state.members.push_back(Member("n-desk", "desk:6680", MemberSeat::Voter));
    office.controller.OnClusterState(state, "c-office", "n-office", "office:6674");
    auto const listed = std::vector<std::string> { "n-desk", "n-office", "n-laptop" };
    REQUIRE(office.controller.Current().members == listed);

    office.controller.OnFleetProven(Testing::ProvenBeacon(
        FleetSummary { .clusterId = "c-office", .state = FleetState::Established, .nodeId = "n-laptop" }));
    CHECK(office.controller.Current().members == listed);

    // The control: the same member speaking for ANOTHER fleet is listed first.
    office.controller.OnFleetProven(Testing::ProvenBeacon(
        FleetSummary { .clusterId = "c-lab", .state = FleetState::Established, .nodeId = "n-laptop" }));
    CHECK(office.controller.Current().members.front() == "n-laptop");
}

TEST_CASE("A node reads split evidence over the state its own cluster applied and what members announced",
          "[node][formation][controller][split]")
{
    Machine office { "n-office", Minted("c-office", OfficeCreatedAt) };
    auto const lab = [](std::string const& speaker, std::vector<std::string> members) {
        auto summary = FleetSummary { .clusterId = "c-lab", .state = FleetState::Established, .nodeId = speaker };
        summary.memberTotal = members.size();
        summary.members = std::move(members);
        return Testing::ProvenBy(summary, speaker);
    };

    // Nothing applied yet: nothing to verify a claim against.
    CHECK(office.controller.ReadSplit(lab("n-laptop", { "n-lab" })).evidence == Cluster::SplitEvidence::None);

    auto state = ClusterState {};
    state.members.push_back(Member("n-office", "office:6680", MemberSeat::Voter));
    state.members.push_back(Member("n-laptop", "", MemberSeat::Learner, Testing::TestKeyPair("n-laptop").PublicKey()));
    office.controller.OnClusterState(state, "c-office", "n-office", "office:6674");
    CHECK(office.controller.ReadSplit(lab("n-laptop", { "n-lab" })).evidence
          == Cluster::SplitEvidence::TheirSpeakerIsOurLearner);

    // What a member announced counts: n-laptop asked c-lab under n-lab's key, and the lab lists it.
    office.announced.Set({ Cluster::AskedJoinBy {
        .askerId = "n-laptop", .clusterId = "c-lab", .provenKey = Testing::TestKeyPair("n-lab").PublicKey() } });
    CHECK(office.controller.ReadSplit(lab("n-lab", { "n-lab", "n-laptop" })).evidence
          == Cluster::SplitEvidence::WeAskedAndTheyListUs);

    // And a list naming this fleet's machine with no evidence says so, for the operator.
    auto const claimed = office.controller.ReadSplit(lab("n-evil", { "n-office" }));
    CHECK(claimed.evidence == Cluster::SplitEvidence::None);
    CHECK(claimed.claimedMember == "n-office");
}

TEST_CASE("The fleets a node asked are what it hands on, as its record keeps them", "[node][formation][controller]")
{
    // What every announcement carries to the leader is the record's memo, read afresh: an ask made
    // since is in the next one.
    Machine laptop { "n-laptop", Minted("c-laptop", 500) };
    CHECK(laptop.controller.AskedJoins().empty());
    laptop.controller.OnFleetProven(Proven("c-office", FleetState::Solitary, OfficeCreatedAt, "office:6674"));
    laptop.controller.Tick();
    REQUIRE(laptop.controller.AskedJoins().size() == 1);
    CHECK(laptop.controller.AskedJoins() == laptop.controller.Record().askedJoins);
    CHECK(laptop.controller.AskedJoins()[0].clusterId == "c-office");
}

TEST_CASE("A node states its own 0xFC endpoint only where a peer could dial it", "[node][formation][controller]")
{
    // The endpoint makes the node a target a peer holding its key may ask again, and nothing more;
    // one only the dialler reaches -- or a wildcard, which names no machine -- is stated as NONE
    // rather than withholding the whole announcement, as the endpoints a peer needs would.
    struct Row
    {
        std::string_view advertised;
        std::string_view stated;
    };
    auto const rows = std::array {
        Row { .advertised = "10.0.0.5:6674", .stated = "10.0.0.5:6674" },
        Row { .advertised = "laptop.corp.example:6674", .stated = "laptop.corp.example:6674" },
        Row { .advertised = "127.0.0.1:6674", .stated = "" },
        Row { .advertised = "localhost:6674", .stated = "" },
        Row { .advertised = "0.0.0.0:6674", .stated = "" },
        Row { .advertised = "[::]:6674", .stated = "" },
        Row { .advertised = "", .stated = "" },
    };
    for (auto const& row: rows)
    {
        INFO(row.advertised);
        Machine laptop { "n-laptop", Minted("c-laptop", 500), {}, std::string { row.advertised } };
        CHECK(laptop.controller.Current().nodeEndpoint == row.stated);
    }
}

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
    CHECK(laptop.controller.Current().state == FleetState::Pending);                 // a pointer now
    CHECK(laptop.controller.Current().pointsAt.leaderNodeEndpoint == "office:6674"); // at the fleet it asked
    CHECK(laptop.controller.Current().leaderNodeEndpoint.empty());                   // and names no leader of its own
    CHECK(laptop.controller.Current().clusterId == "c-laptop");                      // and keeps running its own cluster
    CHECK(laptop.metrics.Read(IMetricsSink::Counter::FormationYields) == 1);

    // The summary it decided on names no speaker and no key for its leader, so the key that proved it
    // is the only one the chain reaches: the next beat proves the leader's endpoint holds exactly that
    // key, and asks nothing; the one after asks.
    auto const office = Proven("c-office", FleetState::Solitary, OfficeCreatedAt, "office:6674").Summary();
    laptop.probe.AnswerSummary("office:6674", Testing::ProvenBy(office, "c-office"));
    laptop.controller.Tick();
    CHECK(laptop.probe.AskedSummaries() == std::vector<std::string> { "office:6674" });
    CHECK(laptop.enroll.Asked().empty());
    laptop.controller.Tick();
    REQUIRE(laptop.enroll.Asked().size() == 1);
    CHECK(laptop.enroll.Asked()[0].endpoint == "office:6674");
    CHECK(laptop.enroll.Asked()[0].self.role == CompileCacheWire::EnrollRole::Learner);
    CHECK(laptop.enroll.Asked()[0].self.nodeId == "n-laptop");
    CHECK(laptop.enroll.Asked()[0].self.publicKey == laptop.self.publicKey);
    // Its endpoint exactly when the role states one, by the column the responder judges it by.
    CHECK(laptop.enroll.Asked()[0].self.nodeEndpoint
          == (EnrollRoleRowFor(CompileCacheWire::EnrollRole::Learner).statesEndpoint ? laptop.self.nodeEndpoint
                                                                                     : std::string {}));
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

TEST_CASE("A node confined to this machine asks no seed and polls no fleet whether solitary or pending",
          "[node][formation][controller][defaults]")
{
    // Its consensus answers on loopback alone (`ConsensusConfinedToThisMachine`), so a fleet that took
    // it would be told to dial an address that reaches only itself. Every beat is a no-op: the typed
    // seed is never asked, a proven fleet never decided on, and a pending join never polled -- the
    // node stays its own cluster, serving its own scheduler.
    Machine solitary {
        "n-laptop", Minted("c-laptop", 500), { "office:6674" }, std::nullopt, FleetReachability::ThisMachineAlone
    };
    solitary.probe.Answer("office:6674",
                          Testing::ProvenSeed(OfficeSummary("c-office", "office:6674"), Cluster::SeedSource::FleetSeedFlag));
    solitary.controller.OnFleetProven(Proven("c-office", FleetState::Solitary, OfficeCreatedAt, "office:6674"));
    solitary.controller.Tick();
    solitary.controller.Tick();
    CHECK(solitary.probe.Asked().empty());
    CHECK(solitary.controller.Mode() == NodeMode::Solitary);
    CHECK(solitary.store.Saves().empty());

    Machine pending { "n-laptop",
                      Pending("c-laptop", 500, "c-office", "office:6674"),
                      {},
                      std::nullopt,
                      FleetReachability::ThisMachineAlone };
    pending.enroll.Script({ Admitted(OfficeRosterWith("n-laptop")) });
    pending.controller.Tick();
    pending.clock.advance(PendingGiveUpAfter + std::chrono::seconds { 1 });
    pending.controller.Tick();
    CHECK(pending.enroll.AskedEndpoints().empty());
    CHECK(pending.probe.Asked().empty());
    CHECK(pending.controller.Mode() == NodeMode::Pending); // neither admitted nor abandoned
    CHECK(pending.store.Saves().empty());
    CHECK(pending.reform.Requests() == 0);

    // The control: the same two machines, open, ask the seed and take the admission.
    Machine open { "n-laptop", Minted("c-laptop", 500), { "office:6674" } };
    open.probe.Answer("office:6674",
                      Testing::ProvenSeed(OfficeSummary("c-office", "office:6674"), Cluster::SeedSource::FleetSeedFlag));
    open.controller.Tick();
    CHECK(open.probe.Asked().size() == 1);
    Machine admitted { "n-laptop", Pending("c-laptop", 500, "c-office", "office:6674") };
    admitted.enroll.Script({ Admitted(OfficeRosterWith("n-laptop")) });
    admitted.controller.Tick();
    CHECK(admitted.controller.Mode() == NodeMode::Learner);
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

TEST_CASE("Approval records the learner, leaves its old store for the next start, and asks for a reform",
          "[node][formation][controller]")
{
    // The decision is taken while the solitary cluster's tier still runs, so it moves no file: the
    // record names the store still in the root, and the reform's start moves it before any tier opens
    // the directory (`ResumeFormation`). The controller holds no archiver at all.
    Machine laptop { "n-laptop", Pending("c-laptop", 500, "c-office", "office:6674") };
    laptop.enroll.Script({ Admitted(OfficeRosterWith("n-laptop")) });
    laptop.controller.Tick();

    REQUIRE(laptop.store.Saves().size() == 1);
    auto const& learner = laptop.store.Saves()[0];
    CHECK(learner.mode == NodeMode::Learner);
    CHECK(learner.archivePending == std::optional<std::string> { "c-laptop" });
    CHECK_FALSE(learner.joining.has_value());
    REQUIRE(learner.fleet.has_value());
    CHECK(Unwrap(learner.fleet).clusterId == "c-office");
    CHECK(Unwrap(learner.fleet).roster == OfficeRosterWith("n-laptop"));
    CHECK(Unwrap(learner.fleet).createdAtUnixSeconds == OfficeCreatedAt); // what the fleet said when it was asked
    REQUIRE(learner.askedJoins.size() == 1);                              // the memo outlives the approval
    CHECK(learner.askedJoins[0].admitted);                                // and is one no later ask displaces
    CHECK(laptop.controller.Record() == learner);
    CHECK(laptop.reform.Requests() == 1);
}

TEST_CASE("A learner shape refused for the schedulers its fleet leaves it is refused before its record is written",
          "[node][formation][controller][judge]")
{
    // The judge reads the remembered fleet endpoints when it judges, and a learner's schedulers are
    // derived from them. `DissolveInto` therefore remembers the fleet's voters BEFORE the save it is
    // judged in: written after, the judge weighs a learner with no schedulers, passes a shape the
    // reform's own judge then refuses, and leaves a learner record on disk that no start can run.
    auto const pending = Pending("c-laptop", 500, "c-office", "office:6674");
    Machine laptop { "n-laptop", pending };
    auto const shaped = LaptopShapedBy(pending);
    LiveNodeConfig const live { shaped, nullptr };
    StartupShapeJudge const production { live, laptop.endpoints, RefuseRegisteringWithTheOffice };
    laptop.judge.DelegateTo(production);
    laptop.enroll.Script({ Admitted(OfficeRosterWith("n-laptop")) });
    laptop.controller.Tick();

    CHECK(laptop.controller.Mode() == NodeMode::Pending);
    CHECK(laptop.store.Saves().empty());
    CHECK(laptop.reform.Requests() == 0);
    CHECK(Logged(laptop.logger, "a test rule refuses registering with office:6674"));
    CHECK(laptop.conditions.StateOf(NodeCondition::FormationMoveRefused) == CompileCacheWire::ConditionState::Raised);
}

TEST_CASE("A learner shape the PRODUCTION advertise rows refuse for its fleet's schedulers is refused before its "
          "record is written",
          "[node][formation][controller][judge]")
{
    // M-J on the merged tree: the production table's fourth advertise row reads where the worker
    // registers (`SchedulersOf`), which for a learner only the remembered fleet endpoints say. A
    // laptop that TYPED a loopback node port is fine pending -- it registers with its own scheduler,
    // on this machine -- and would register LOOPBACK with the office's once it moves. Judged on the
    // endpoints the move leaves behind, the move is refused before its record is written: no
    // learner record on disk that every restart then refuses.
    auto const pending = Pending("c-laptop", 500, "c-office", "office:6674");
    Machine laptop { "n-laptop", pending };
    auto shaped = LaptopShapedBy(pending);
    shaped.nodeListen = "127.0.0.1:6674";
    shaped.nodeListenExplicit = true;
    shaped.toolchains = { "/usr/bin/g++" };
    INFO("pending refusal: " << StartupPolicyRejection(shaped).value_or("<none>"));
    REQUIRE_FALSE(StartupPolicyRejection(shaped).has_value()); // the shape it is in starts
    LiveNodeConfig const live { shaped, nullptr };
    StartupShapeJudge const production { live, laptop.endpoints, StartupPolicyRejection };
    laptop.judge.DelegateTo(production);
    laptop.enroll.Script({ Admitted(OfficeRosterWith("n-laptop")) });
    laptop.controller.Tick();

    CHECK(laptop.controller.Mode() == NodeMode::Pending);
    CHECK(laptop.store.Saves().empty());
    CHECK(laptop.reform.Requests() == 0);
    CHECK(Logged(laptop.logger, "would register LOOPBACK"));
    CHECK(laptop.conditions.StateOf(NodeCondition::FormationMoveRefused) == CompileCacheWire::ConditionState::Raised);
}

TEST_CASE("An admission whose roster does not record this node under its key is not believed",
          "[node][formation][controller]")
{
    // The roster admits a DIFFERENT machine under that id's key: the fleet answered, signed, and an
    // operator approved another key for this id -- joining on it would be a confident wrong move.
    Machine laptop { "n-laptop", Pending("c-laptop", 500, "c-office", "office:6674") };
    laptop.enroll.Script({ Admitted(OfficeRosterWith("n-someone-else")) });
    laptop.controller.Tick();
    CHECK(laptop.controller.Mode() == NodeMode::Pending);
    CHECK(laptop.store.Saves().empty());
    CHECK(Logged(laptop.logger, "does not record n-laptop"));

    // The control: the roster that does record it is believed.
    laptop.enroll.Script({ Admitted(OfficeRosterWith("n-laptop")) });
    laptop.controller.Tick();
    CHECK(laptop.controller.Mode() == NodeMode::Learner);
}

TEST_CASE("An admission whose roster no key of the proven fleet vouches for is refused, and the node stays put",
          "[node][formation][controller]")
{
    // Signed by the key that proved the fleet, over this request -- the fleet answered -- but with a
    // roster that names none of the proven fleet's keys: the fleet answering wrongly, which is counted
    // apart from an answer nobody can bind to the fleet.
    Machine laptop { "n-laptop", Pending("c-laptop", 500, "c-office", "office:6674") };
    laptop.enroll.Script({ Admitted(Testing::RosterWith("n-evil", "n-laptop")) });
    laptop.controller.Tick();

    CHECK(laptop.controller.Mode() == NodeMode::Pending);
    CHECK(laptop.controller.Record().own.clusterId == "c-laptop"); // its own cluster, untouched
    CHECK(laptop.store.Saves().empty());
    CHECK(laptop.reform.Requests() == 0);
    CHECK(laptop.metrics.Read(IMetricsSink::Counter::FormationAdmissionsRefused) == 1);
    CHECK(laptop.metrics.Read(IMetricsSink::Counter::FormationAdmissionsUnverified) == 0);
    CHECK(Logged(laptop.logger, "records no member under the key that proved the fleet c-office"));

    // The control: the office's own roster is believed.
    laptop.enroll.Script({ Admitted(OfficeRosterWith("n-laptop")) });
    laptop.controller.Tick();
    CHECK(laptop.controller.Mode() == NodeMode::Learner);
    CHECK(laptop.metrics.Read(IMetricsSink::Counter::FormationAdmissionsRefused) == 1);
}

TEST_CASE("An admission copied from public facts is refused unless the key this node proved signed it",
          "[node][formation][controller][enroll]")
{
    // The reviewer's probes T1 and T2: n-evil answers at the endpoint polled with a roster naming the
    // office's member under its PUBLIC key -- seated as a learner (T1) or as a voter (T2) -- and this
    // node. Every fact in it rides every beacon, so a check of its content believed both. What stops
    // them is that nothing n-evil holds signs as the key this node proved for c-office: unsigned, or
    // signed with its own key, the answer is refused by name, counted, and the node stays put.
    struct Probe
    {
        std::string_view name; ///< Which probe.
        MemberSeat seat;       ///< Where the copied key is seated.
        bool signs;            ///< Whether n-evil signs with its own key rather than not at all.
    };
    for (auto const& probe: { Probe { .name = "T1 unsigned", .seat = MemberSeat::Learner, .signs = false },
                              Probe { .name = "T1 signed by n-evil", .seat = MemberSeat::Learner, .signs = true },
                              Probe { .name = "T2 unsigned", .seat = MemberSeat::Voter, .signs = false },
                              Probe { .name = "T2 signed by n-evil", .seat = MemberSeat::Voter, .signs = true } })
    {
        INFO(probe.name);
        Machine laptop { "n-laptop", Pending("c-laptop", 500, "c-office", "office:6674") };
        auto const roster = CopiedRoster(probe.seat);
        laptop.enroll.Script({ probe.signs ? AdmittedBy("n-evil", "c-office", roster)
                                           : Testing::ScriptedAnswer { UnsignedAdmission(roster) } });
        laptop.controller.Tick();

        CHECK(laptop.controller.Mode() == NodeMode::Pending);
        CHECK(laptop.controller.Record().own.clusterId == "c-laptop"); // its own cluster, untouched
        CHECK(laptop.store.Saves().empty());
        CHECK(laptop.reform.Requests() == 0);
        CHECK(laptop.metrics.Read(IMetricsSink::Counter::FormationAdmissionsUnverified) == 1);
        CHECK(laptop.metrics.Read(IMetricsSink::Counter::FormationAdmissionsRefused) == 0);
        CHECK(Logged(
            laptop.logger,
            Cluster::WordsFor(probe.signs ? Cluster::AdmissionSignature::Unproven : Cluster::AdmissionSignature::Unsigned)));

        // The control: the same roster's shape, signed by the key that proved the fleet, is believed
        // -- so what refused the probe is the signature, not the roster.
        laptop.enroll.Script({ Admitted(OfficeRosterWith("n-laptop")) });
        laptop.controller.Tick();
        CHECK(laptop.controller.Mode() == NodeMode::Learner);
    }
}

TEST_CASE("An admission is asked for over a fresh nonce, from the leader the node proved, and no probe is spent on it",
          "[node][formation][controller][enroll]")
{
    // The summary the join was decided on says its speaker leads, so the leader's endpoint is the
    // speaker's own and its key is the one already proved: nothing is asked of it first.
    Machine laptop { "n-laptop", Pending("c-laptop", 500, "c-office", "office:6674") };
    laptop.enroll.Script({ Waiting(), Admitted(OfficeRosterWith("n-laptop")) });
    laptop.controller.Tick();
    laptop.controller.Tick();

    CHECK(laptop.controller.Mode() == NodeMode::Learner);
    CHECK(laptop.probe.AskedSummaries().empty());
    REQUIRE(laptop.enroll.Asked().size() == 2);
    CHECK(laptop.enroll.Asked()[0].nonce.size() == CompileCacheWire::NodeChallengeBytes);
    CHECK(laptop.enroll.Asked()[0].nonce != laptop.enroll.Asked()[1].nonce); // one per request
    CHECK(laptop.metrics.Read(IMetricsSink::Counter::FormationAdmissionsUnverified) == 0);
}

TEST_CASE("An admission recorded from another ask is refused, though the fleet's own key signed it",
          "[node][formation][controller][enroll]")
{
    // A replay: the office's genuine admission, signed over the nonce of an EARLIER request, handed
    // back to a later one. Its key is the right one and its roster is the right one; only the nonce
    // this node drew for this request tells it apart.
    Machine laptop { "n-laptop", Pending("c-laptop", 500, "c-office", "office:6674") };
    laptop.enroll.Script({ Waiting(),
                           Testing::ScriptedAnswer { UnsignedAdmission(OfficeRosterWith("n-laptop")),
                                                     Testing::AdmissionSigning {
                                                         .signer = "n-office", .clusterId = "c-office", .overPoll = 0 } } });
    laptop.controller.Tick();
    laptop.controller.Tick();

    REQUIRE(laptop.enroll.Asked().size() == 2);
    REQUIRE(laptop.enroll.Asked()[0].nonce != laptop.enroll.Asked()[1].nonce);
    CHECK(laptop.controller.Mode() == NodeMode::Pending);
    CHECK(laptop.store.Saves().empty());
    CHECK(laptop.metrics.Read(IMetricsSink::Counter::FormationAdmissionsUnverified) == 1);
    CHECK(Logged(laptop.logger, Cluster::WordsFor(Cluster::AdmissionSignature::Forged)));
}

TEST_CASE("A leader a redirect names is proved before it is asked, and its admission is then believed",
          "[node][formation][controller][enroll]")
{
    // n-office answers `NotLeader` naming desk:6674. A redirect is not signed, so the node asks the
    // office -- whose key proved the fleet -- which key leads now, and proves desk holds EXACTLY that
    // key, on one beat, and asks it on the next; n-desk's admission is then held to n-desk's key.
    Machine laptop { "n-laptop", Pending("c-laptop", 500, "c-office", "office:6674") };
    auto desk = OfficeSummary("c-office", "desk:6674");
    desk.leaderId = "n-desk";
    desk.nodeId = "n-desk";
    laptop.probe.AnswerSummary("office:6674", OfficeNamingLeader("n-desk"));
    laptop.probe.AnswerSummary("desk:6674", Testing::ProvenBy(desk, "n-desk"));
    laptop.enroll.Script({ Redirect("desk:6674"), AdmittedBy("n-desk", "c-office", OfficeRosterWith("n-laptop")) });

    laptop.controller.Tick(); // the office redirects
    CHECK(laptop.probe.AskedSummaries().empty());
    laptop.controller.Tick(); // the office names n-desk's key, desk proves it, and is not asked yet
    CHECK(laptop.probe.AskedSummaries() == std::vector<std::string> { "office:6674", "desk:6674" });
    CHECK(laptop.enroll.AskedEndpoints() == std::vector<std::string> { "office:6674" });
    CHECK(laptop.controller.Mode() == NodeMode::Pending);

    laptop.controller.Tick(); // desk admits
    CHECK(laptop.enroll.AskedEndpoints() == std::vector<std::string> { "office:6674", "desk:6674" });
    CHECK(laptop.controller.Mode() == NodeMode::Learner);
    CHECK(laptop.metrics.Read(IMetricsSink::Counter::FormationAdmissionsUnverified) == 0);
}

TEST_CASE("A redirected leader is held to the key proved there, not to the key the join was decided on",
          "[node][formation][controller][enroll]")
{
    // The key proved at desk:6674 is n-desk's; an admission from there signed by n-office's key -- the
    // one the join was decided on -- is some other machine's signature relayed through desk, and is
    // not believed from desk.
    Machine laptop { "n-laptop", Pending("c-laptop", 500, "c-office", "office:6674") };
    auto desk = OfficeSummary("c-office", "desk:6674");
    desk.leaderId = "n-desk";
    desk.nodeId = "n-desk";
    laptop.probe.AnswerSummary("office:6674", OfficeNamingLeader("n-desk"));
    laptop.probe.AnswerSummary("desk:6674", Testing::ProvenBy(desk, "n-desk"));
    laptop.enroll.Script({ Redirect("desk:6674"), Admitted(OfficeRosterWith("n-laptop")) });
    for ([[maybe_unused]] auto const beat: std::views::iota(0, 3))
        laptop.controller.Tick();

    CHECK(laptop.enroll.AskedEndpoints() == std::vector<std::string> { "office:6674", "desk:6674" });
    CHECK(laptop.controller.Mode() == NodeMode::Pending);
    CHECK(laptop.metrics.Read(IMetricsSink::Counter::FormationAdmissionsUnverified) == 1);
    CHECK(Logged(laptop.logger, Cluster::WordsFor(Cluster::AdmissionSignature::Unproven)));
}

TEST_CASE("A redirect to an endpoint that proves another fleet, or nothing, is never asked",
          "[node][formation][controller][enroll]")
{
    // Where a redirect points is proved first, and an endpoint that proves no key for c-office is not
    // asked at all: it has no key an admission could be held to. Such a beat reads as the fleet not
    // answering, which is what the give-up counts -- it first asks the office which key leads, since
    // the redirect named none -- and the next poll goes back to the office.
    struct Target
    {
        std::string_view name;                                 ///< The case.
        std::optional<CompileCacheWire::FleetSummary> answers; ///< What it proves, or nothing.
    };
    auto evil = OfficeSummary("c-evil", "evil:6674");
    evil.leaderId = "n-evil";
    evil.nodeId = "n-evil";
    for (auto const& target: { Target { .name = "another fleet's key", .answers = evil },
                               Target { .name = "no answer", .answers = std::nullopt } })
    {
        INFO(target.name);
        Machine laptop { "n-laptop", Pending("c-laptop", 500, "c-office", "office:6674") };
        laptop.probe.AnswerSummary("office:6674", OfficeNamingLeader("n-office"));
        if (target.answers.has_value())
            laptop.probe.AnswerSummary("evil:6674", Testing::ProvenBy(Unwrap(target.answers), "n-evil"));
        laptop.enroll.Script({ Redirect("evil:6674") });
        laptop.enroll.ScriptForever(Waiting());
        for ([[maybe_unused]] auto const beat: std::views::iota(0, 3))
            laptop.controller.Tick();

        CHECK(laptop.enroll.AskedEndpoints() == std::vector<std::string> { "office:6674", "office:6674" });
        CHECK(laptop.probe.AskedSummaries() == std::vector<std::string> { "office:6674", "evil:6674" });
        CHECK(laptop.controller.Mode() == NodeMode::Pending);
        CHECK(Logged(laptop.logger,
                     target.answers.has_value() ? "proves a key for cluster c-evil, not for c-office"
                                                : "cannot prove who answers at evil:6674"));
    }
}

TEST_CASE("A poll whose nonce cannot be drawn is not sent", "[node][formation][controller][enroll]")
{
    // A nonce a relay could predict is one whose answer it could have recorded from another ask, so a
    // generator that fails sends nothing: the beat reads as the fleet not answering, and says why.
    Machine laptop { "n-laptop", Pending("c-laptop", 500, "c-office", "office:6674") };
    laptop.random.Deny(Testing::ScriptedSecureRandom::DeniedFailure());
    laptop.enroll.ScriptForever(Admitted(OfficeRosterWith("n-laptop")));
    laptop.controller.Tick();

    CHECK(laptop.enroll.Asked().empty());
    CHECK(laptop.controller.Mode() == NodeMode::Pending);
    CHECK(Logged(laptop.logger, "no nonce could be drawn to ask office:6674"));
    CHECK(Logged(laptop.logger, "scripted-getrandom"));
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

    // The join is cleared and its memo is KEPT: the evidence a split is told on outlives the ask.
    auto const record = laptop.controller.Record();
    CHECK_FALSE(record.joining.has_value());
    REQUIRE(record.askedJoins.size() == 1);
    CHECK(record.askedJoins[0].clusterId == "c-office");
}

TEST_CASE("A redirect target that proves nothing sends the next poll back to the endpoint the join was decided on",
          "[node][formation][controller][enroll][chain]")
{
    // The office redirects to desk:6674, and the office -- the chain's anchor -- cannot say which key
    // leads there, so desk proves nothing. Retried there until the give-up, one injected redirect, or
    // a leader briefly unreachable, would cost the whole join; the next poll goes back to the office
    // instead, and the give-up clock the redirect started keeps running.
    Machine laptop { "n-laptop", Pending("c-laptop", 500, "c-office", "office:6674") };
    laptop.enroll.Script({ Redirect("desk:6674"), Waiting() });
    laptop.controller.Tick(); // the office redirects
    laptop.controller.Tick(); // desk cannot be proved: nothing is polled, and the chain goes back
    CHECK(laptop.probe.AskedSummaries() == std::vector<std::string> { "office:6674" });
    laptop.controller.Tick(); // the office, asked again under the root key
    CHECK(laptop.enroll.AskedEndpoints() == std::vector<std::string> { "office:6674", "office:6674" });
    CHECK(laptop.controller.Mode() == NodeMode::Pending);
    CHECK(Logged(laptop.logger, "asking office:6674 again"));
}

TEST_CASE("A redirect is followed and a chain longer than the bound goes back to the target",
          "[node][formation][controller]")
{
    // Each hop is proved before it is asked -- a beat of its own, which first asks the office which
    // key leads -- and the chain back to the target needs no proof: the key that proved the fleet is
    // its leader's own. Every hop answers under the key the office names, n-desk's.
    Machine laptop { "n-laptop", Pending("c-laptop", 500, "c-office", "office:6674") };
    laptop.probe.AnswerSummary("office:6674", OfficeNamingLeader("n-desk"));
    for (auto const* hop: { "a", "b", "c" })
    {
        auto summary = OfficeSummary("c-office", std::format("{}:6674", hop));
        summary.leaderId = "n-desk";
        summary.nodeId = "n-desk";
        laptop.probe.AnswerSummary(std::format("{}:6674", hop), Testing::ProvenBy(summary, "n-desk"));
    }
    laptop.enroll.Script({ Redirect("a:6674"), Redirect("b:6674"), Redirect("c:6674"), Redirect("d:6674"), Waiting() });
    for ([[maybe_unused]] auto const beat: std::views::iota(0, 8))
        laptop.controller.Tick();
    CHECK(laptop.enroll.AskedEndpoints()
          == std::vector<std::string> { "office:6674", "a:6674", "b:6674", "c:6674", "office:6674" });
    CHECK(laptop.probe.AskedSummaries()
          == std::vector<std::string> { "office:6674", "a:6674", "office:6674", "b:6674", "office:6674", "c:6674" });
    static_assert(MaxEnrollRedirects == 3, "the script above follows exactly MaxEnrollRedirects hops");
    CHECK(laptop.controller.Mode() == NodeMode::Pending);
}

TEST_CASE("A move whose shape the startup rules refuse is not taken, though it would not reform",
          "[node][formation][controller][judge]")
{
    // Solitary to voter rebuilds nothing, so no reform would ever judge the shape: the move itself is
    // judged, before its record is written, and a refusal leaves the node where it is -- said once,
    // and raised as a condition naming the rule.
    Machine office { "n-office", Minted("c-office", OfficeCreatedAt) };
    REQUIRE(office.conditions.StateOf(NodeCondition::FormationMoveRefused) == CompileCacheWire::ConditionState::Clear);
    constexpr auto Rule = std::string_view { "the rule a voter's shape breaks" };
    office.judge.Refuse(NodeMode::Voter, std::string { Rule });

    auto state = ClusterState {};
    state.members.push_back(Member("n-office", "office:6680", MemberSeat::Voter));
    state.members.push_back(Member("n-laptop", "", MemberSeat::Learner));
    office.controller.OnClusterState(state, "c-office", "n-office", "office:6674");
    office.controller.OnClusterState(state, "c-office", "n-office", "office:6674"); // asked again: said once

    CHECK(office.controller.Mode() == NodeMode::Solitary);
    CHECK(office.store.Saves().empty());
    CHECK(office.reform.Requests() == 0);
    CHECK(office.judge.Asked() == std::vector { NodeMode::Voter, NodeMode::Voter });
    REQUIRE(office.conditions.StateOf(NodeCondition::FormationMoveRefused) == CompileCacheWire::ConditionState::Raised);
    auto const detail = Testing::DetailOf(office.conditions, NodeCondition::FormationMoveRefused);
    CHECK(detail.contains(Rule));
    CHECK(detail.contains("staying solitary rather than move to voter"));
    auto const warned = std::ranges::count_if(office.logger.Snapshot(), [&](CapturingLogger::Record const& record) {
        return record.level == LogLevel::Warn && record.message.contains(Rule);
    });
    CHECK(warned == 1);
    CHECK_FALSE(std::ranges::any_of(office.logger.Snapshot(),
                                    [](CapturingLogger::Record const& record) { return record.level == LogLevel::Error; }));

    // The rule satisfied, the next applied state moves it, and the row clears.
    office.judge.AcceptAll();
    office.controller.OnClusterState(state, "c-office", "n-office", "office:6674");
    CHECK(office.controller.Mode() == NodeMode::Voter);
    CHECK(office.conditions.StateOf(NodeCondition::FormationMoveRefused) == CompileCacheWire::ConditionState::Clear);
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
    auto const record = laptop.controller.Record();
    CHECK(record.archivePending == std::optional<std::string> { "c-office" }); // moved at the reform's start
    CHECK(record.own.clusterId != "c-laptop");                                 // a NEW cluster, never the old one back
    CHECK(record.own.clusterId != "c-office");
    CHECK(record.own.createdAtUnixSeconds == StartSeconds);
    CHECK_FALSE(record.fleet.has_value());
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

    // A seat recorded under another key -- the only keyless-looking case left, since a recorded
    // member always holds a key.
    auto seatedUnderAnotherKey = ClusterState {};
    seatedUnderAnotherKey.members = {
        office, Member("n-laptop", "laptop:6680", MemberSeat::Voter, Testing::TestKeyPair("n-impostor").PublicKey())
    };
    laptop.controller.OnClusterState(seatedUnderAnotherKey, "c-office", "n-office", "office:6674");
    CHECK(laptop.controller.Mode() == NodeMode::Learner);
    CHECK(Logged(laptop.logger, "without this machine's key"));
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

TEST_CASE("What a node says about itself is answered while its move waits on the disk", "[node][formation][controller]")
{
    // Every beacon and proof reads `Current`, so a slow disk under a move must not stall discovery:
    // the move's save runs outside the lock `Current` takes, and what the store has already kept is
    // what `Current` says.
    Machine laptop { "n-laptop", Pending("c-laptop", 500, "c-office", "office:6674") };
    laptop.store.HoldSaves();
    laptop.enroll.Script({ Admitted(OfficeRosterWith("n-laptop")) });

    // Declared so they unwind in the order that frees everything: the release first, then the
    // answer it unblocks, then the beat it lets finish.
    std::jthread beat { [&laptop] { laptop.controller.Tick(); } };
    auto answer = std::future<FleetSummary> {};
    ReleaseOnExit const release { laptop.store };

    REQUIRE(Testing::WaitUntil(
        "the approval's save to be entered",
        [&laptop] { return laptop.store.SaveEntered(); },
        [] { return std::string { "the beat has not reached the save" }; }));
    answer = std::async(std::launch::async, [&laptop] { return laptop.controller.Current(); });
    REQUIRE(Testing::WaitUntil(
        "Current() to answer while the save is held",
        [&answer] { return answer.wait_for(std::chrono::seconds { 0 }) == std::future_status::ready; },
        [] { return std::string { "Current() is still waiting" }; }));

    // OBSERVED, not timed: the save is still being held now that Current() has answered, so it
    // answered past a move in progress -- not because the hold ran out and the move finished. The hold
    // outlasts the wait above by far, which is what lets this fail at all.
    STATIC_REQUIRE(Testing::InMemoryFormationStore::HeldSaveBound > 2 * Testing::WaitHangGuard);
    CHECK(laptop.store.SaveHeldNow());

    // It answers with what the store has kept so far: nothing of the move yet, so still pending in
    // its own cluster.
    auto const said = answer.get();
    CHECK(said.clusterId == "c-laptop");
    CHECK(said.state == FleetState::Pending);
}

TEST_CASE("A state decided about the cluster a node is leaving is dropped when the node has moved before it fires",
          "[node][formation][controller]")
{
    // Decided and fired are two moments. A state of the solitary cluster arrives while the approval's
    // move is on the disk: it passes the tag -- the record still says c-laptop -- and waits for the
    // move. Fired after it, its SeatedVoter would read the LEARNER row and seat the node as a voter of
    // a fleet that never seated it, with a second reform.
    Machine laptop { "n-laptop", Pending("c-laptop", 500, "c-office", "office:6674") };
    laptop.store.HoldSaves();
    laptop.enroll.Script({ Admitted(OfficeRosterWith("n-laptop")) });
    auto own = ClusterState {};
    own.members.push_back(Member("n-laptop", "laptop:6680", MemberSeat::Voter, laptop.self.publicKey));

    // Declared so they unwind in the order that frees everything: the release first, then the two
    // threads it lets finish.
    std::jthread beat { [&laptop] { laptop.controller.Tick(); } };
    std::jthread stale;
    ReleaseOnExit const release { laptop.store };

    REQUIRE(Testing::WaitUntil(
        "the approval's save to be entered",
        [&laptop] { return laptop.store.SaveEntered(); },
        [] { return std::string { "the beat has not reached the save" }; }));
    stale =
        std::jthread { [&laptop, &own] { laptop.controller.OnClusterState(own, "c-laptop", "n-laptop", "laptop:6674"); } };
    // Decided once the state is taken on, which happens under the lock, before it waits for the move
    // -- and seen where production reads the applied state: the split evidence it now records, the
    // laptop as a voter under its own key, speaking for another fleet.
    auto const speaking = Testing::ProvenBy(
        FleetSummary { .clusterId = "c-other", .state = FleetState::Established, .nodeId = "n-laptop" }, "n-laptop");
    REQUIRE(Testing::WaitUntil(
        "the stale state to be decided",
        [&laptop, &speaking] {
            return laptop.controller.ReadSplit(speaking).evidence == Cluster::SplitEvidence::TheirSpeakerIsOurVoter;
        },
        [] { return std::string { "the state has not been taken on" }; }));
    laptop.store.ReleaseSaves();
    beat.join();
    stale.join();

    CHECK(laptop.controller.Mode() == NodeMode::Learner);
    CHECK(laptop.reform.Requests() == 1);
    REQUIRE(laptop.controller.Record().fleet.has_value());
    CHECK(Unwrap(laptop.controller.Record().fleet).clusterId == "c-office");
    CHECK(Logged(laptop.logger, "dropping"));
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
    CHECK(laptop.controller.Record().archivePending == std::optional<std::string> { "c-office" });
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

namespace
{
/// The office's applied state: its founder `n-office` and `n-desk` voters, and `n-laptop` a learner,
/// each under its own key.
/// @return The state.
[[nodiscard]] ClusterState OfficeWithLaptop()
{
    auto state = ClusterState {};
    state.members.push_back(
        Member("n-office", "office:6680", MemberSeat::Voter, Testing::TestKeyPair("n-office").PublicKey()));
    state.members.push_back(Member("n-desk", "desk:6680", MemberSeat::Voter, Testing::TestKeyPair("n-desk").PublicKey()));
    state.members.push_back(Member("n-laptop", "", MemberSeat::Learner, Testing::TestKeyPair("n-laptop").PublicKey()));
    return state;
}

/// What `c-home` says, established and created at @p created, spoken by @p speaker under its own key.
/// @param speaker Who speaks for it.
/// @param created When it was created.
/// @return The proven fleet, as a beacon proves it.
[[nodiscard]] Cluster::ProvenFleet Home(std::string const& speaker, std::uint64_t created)
{
    return Testing::ProvenBeacon(FleetSummary { .clusterId = "c-home",
                                                .state = FleetState::Established,
                                                .createdAtUnixSeconds = created,
                                                .leaderId = "n-home",
                                                .leaderNodeEndpoint = "home:6674",
                                                .nodeId = speaker,
                                                .raftEndpoint = "",
                                                .members = { "n-home", speaker, "n-office" },
                                                .memberTotal = 3,
                                                .nodeEndpoint = "",
                                                .leaderKey = Testing::TestKeyPair("n-home").PublicKey(),
                                                .pointsAt = {} });
}

/// The office's founder, a voter leading `c-office` (created at 200) with the laptop as a learner.
/// @param machine The founder's machine.
/// @param leader Who leads, as the applied state says.
void Lead(Machine& machine, std::string_view leader = "n-office")
{
    machine.controller.OnClusterState(OfficeWithLaptop(), "c-office", Consensus::NodeId { leader }, "office:6674");
    REQUIRE(machine.controller.Mode() == NodeMode::Voter);
}

/// The dissolve order a leader of `c-office` replicates for `c-home`, proven under the laptop's key,
/// whose summary named n-home's key for the survivor's leader.
/// @return The state carrying it.
[[nodiscard]] ClusterState OfficeDissolvingIntoHome()
{
    auto state = OfficeWithLaptop();
    state.dissolveOrder = Cluster::DissolveOrder { .clusterId = "c-home",
                                                   .provenKey = Testing::TestKeyPair("n-laptop").PublicKey(),
                                                   .leaderNodeEndpoint = "home:6674",
                                                   .createdAtUnixSeconds = 100,
                                                   .leaderKey = Testing::TestKeyPair("n-home").PublicKey() };
    return state;
}
} // namespace

TEST_CASE("A fleet's leader proposes its dissolve into an older split of it, on evidence a key verifies",
          "[node][formation][controller][split]")
{
    // The desk is a VOTER here under its key, and speaks for c-home -- evidence (A) that heals by itself
    // -- and c-home is older, so this side loses the tiebreak: the leader proposes one dissolve, which
    // every member follows once it is applied.
    Machine office { "n-office", Minted("c-office", 200) };
    Lead(office);
    office.controller.OnFleetProven(Home("n-desk", 100));
    office.controller.Tick();

    REQUIRE(office.admin.Proposed().size() == 1);
    auto const proposed = office.admin.Proposed()[0]; // a copy: `Proposed()` returns one
    CHECK(proposed.kind == Cluster::CommandKind::DissolveInto);
    CHECK(proposed.key == "c-home");
    CHECK(proposed.value == "home:6674");
    CHECK(proposed.publicKey == std::optional { Testing::TestKeyPair("n-desk").PublicKey() });
    CHECK(proposed.createdAtUnixSeconds == std::optional<std::uint64_t> { 100 });
    // The desk spoke, not the leader: the leader's key is the one its signed summary states.
    CHECK(proposed.leaderKey == std::optional { Testing::TestKeyPair("n-home").PublicKey() });
    CHECK(Cluster::Validate(proposed).has_value());
    CHECK(Logged(office.logger, "proposed dissolving this fleet into c-home"));
    CHECK(office.controller.Mode() == NodeMode::Voter); // it moves when the ORDER applies, with every member

    // Once per interval: proven again on the next beat, it is not proposed again until the interval
    // has passed -- a proposal that did not commit is repeated, never flooded.
    office.controller.OnFleetProven(Home("n-desk", 100));
    office.controller.Tick();
    CHECK(office.admin.Proposed().size() == 1);
    office.clock.advance(SeedProbeInterval);
    office.controller.OnFleetProven(Home("n-desk", 100));
    office.controller.Tick();
    CHECK(office.admin.Proposed().size() == 2);
}

TEST_CASE("A seed's answer a solitary beat queued is not read as split evidence once the node is a member",
          "[node][formation][controller][split]")
{
    // A solitary beat asks a typed seed and decides on the NEXT beat. A node whose cluster records a
    // second machine in between is a voter at that beat, and the answer -- asked for as a fleet to
    // JOIN -- would be its first member beat's evidence that its own fleet is split: here the desk, a
    // voter of this fleet under its own key, speaking for an older c-home, which is exactly what makes
    // the leader propose its dissolve when a beacon proves it ("A fleet's leader proposes its
    // dissolve..."). Each mode decides only what it queued.
    Machine office { "n-office", Minted("c-office", 200), { "home:6674" } };
    office.probe.Answer("home:6674", Testing::ProvenSeed(Home("n-desk", 100).Summary(), Cluster::SeedSource::FleetSeedFlag));
    office.controller.Tick(); // asks the seed and queues its answer; decides nothing on this beat
    REQUIRE(office.probe.Asked().size() == 1);
    REQUIRE(office.controller.Mode() == NodeMode::Solitary);

    Lead(office);
    office.controller.Tick();
    CHECK(office.admin.Proposed().empty());
    CHECK(office.controller.Mode() == NodeMode::Voter);

    // The control: the same fleet proven AFTER the move is the member beat's to read, and it is.
    office.controller.OnFleetProven(Home("n-desk", 100));
    office.controller.Tick();
    CHECK(office.admin.Proposed().size() == 1);
}

TEST_CASE("A fleet's leader never proposes a dissolve on a claim, when it wins, or while another leads",
          "[node][formation][controller][split]")
{
    SECTION("an older fleet listing this fleet's leader, spoken by a key this fleet never held")
    {
        // The attack a member list invites: ids are public, so an older fleet can list n-office. With no
        // key of this fleet behind the claim it is foreign, and a foreign fleet is never yielded to.
        Machine office { "n-office", Minted("c-office", 200) };
        Lead(office);
        office.controller.OnFleetProven(Home("n-stranger", 1));
        office.controller.Tick();
        CHECK(office.admin.Proposed().empty());
    }
    SECTION("a younger split of this fleet: this side wins, and the other side leaves")
    {
        Machine office { "n-office", Minted("c-office", 200) };
        Lead(office);
        office.controller.OnFleetProven(Home("n-desk", 300));
        office.controller.Tick();
        CHECK(office.admin.Proposed().empty());
    }
    SECTION("a voter that does not lead: only the leader decides for the fleet")
    {
        Machine office { "n-office", Minted("c-office", 200) };
        Lead(office, "n-desk");
        office.controller.OnFleetProven(Home("n-desk", 100));
        office.controller.Tick();
        CHECK(office.admin.Proposed().empty());
    }
}

TEST_CASE("A learner's word that this fleet is split is told and never moves the fleet",
          "[node][formation][controller][split][security]")
{
    // A learner is a machine auto-approval admitted on first use; one running a second process under
    // its own key could mint an older fleet and speak for it. So its (A) is read -- the operator is
    // told, naming it -- and no dissolve is proposed on it.
    Machine office { "n-office", Minted("c-office", 200) };
    Lead(office);
    auto const byLearner = Home("n-laptop", 100);
    auto const reading = office.controller.ReadSplit(byLearner.Proven());
    CHECK(reading.evidence == Cluster::SplitEvidence::TheirSpeakerIsOurLearner);
    CHECK(reading.witness == "n-laptop");
    office.controller.OnFleetProven(byLearner);
    office.controller.Tick();
    CHECK(office.admin.Proposed().empty());

    // The control, which the first case pins in full: the same fleet spoken for by a VOTER is healed.
    office.controller.OnFleetProven(Home("n-desk", 100));
    office.controller.Tick();
    CHECK(office.admin.Proposed().size() == 1);
}

namespace
{
/// `c-mint`: a fleet anybody could mint, established since 1 -- older than any fleet -- its leader
/// `n-mint` speaking for it at `mint:6674` under its own key, listing @p members.
/// @param members Whom it lists.
/// @return The proven fleet, as a beacon proves it.
[[nodiscard]] Cluster::ProvenFleet Mint(std::vector<std::string> members)
{
    auto const total = members.size();
    return Testing::ProvenBeacon(FleetSummary { .clusterId = "c-mint",
                                                .state = FleetState::Established,
                                                .createdAtUnixSeconds = 1,
                                                .leaderId = "n-mint",
                                                .leaderNodeEndpoint = "mint:6674",
                                                .nodeId = "n-mint",
                                                .raftEndpoint = "",
                                                .members = std::move(members),
                                                .memberTotal = total,
                                                .nodeEndpoint = "",
                                                .leaderKey = Testing::TestKeyPair("n-mint").PublicKey(),
                                                .pointsAt = {} });
}

/// The memos @p asker hands its fleet's leader, as NODE-ANNOUNCE carries them: each under the id it
/// proves.
/// @param asker The machine whose memos they are.
/// @return Its memos, announced.
[[nodiscard]] std::vector<Cluster::AskedJoinBy> AnnouncedBy(Machine const& asker)
{
    auto announced = std::vector<Cluster::AskedJoinBy> {};
    for (auto const& memo: asker.controller.AskedJoins())
        announced.push_back(
            Cluster::AskedJoinBy { .askerId = asker.self.nodeId, .clusterId = memo.clusterId, .provenKey = memo.provenKey });
    return announced;
}

/// Make @p office lead the office holding @p announced from its learner the laptop, and give it one
/// beat over c-mint listing the laptop -- after checking that the evidence (C) IS read.
/// @param office The office's founder.
/// @param announced What the laptop announced.
void OfficeSeesMintListingLaptop(Machine& office, std::vector<Cluster::AskedJoinBy> announced)
{
    Lead(office);
    office.announced.Set(std::move(announced));
    auto const listing = Mint({ "n-mint", "n-laptop" });
    // The evidence IS there: what this case shows is that it moves nothing, not that it was absent.
    auto const reading = office.controller.ReadSplit(listing.Proven());
    CHECK(reading.evidence == Cluster::SplitEvidence::WeAskedAndTheyListUs);
    CHECK(reading.witness == "n-laptop");
    office.controller.OnFleetProven(listing);
    office.controller.Tick();
}
} // namespace

TEST_CASE("A fleet a machine here once asked and that refused it or never answered is never yielded to however old",
          "[node][formation][controller][split][security]")
{
    // The takeover a memo invites. A fresh laptop asks whichever established fleet beacons oldest --
    // c-mint, which anybody can mint -- and so holds a memo under c-mint's key. c-mint refuses it, or
    // never answers; the laptop then joins the office. When c-mint later lists the laptop, the office's
    // leader reads evidence (C) -- under a key this fleet came to hold only on that first-use trust --
    // and must not dissolve the office, voters and all, into c-mint.
    Machine laptop { "n-laptop", Minted("c-laptop", 500) };
    laptop.controller.OnFleetProven(Mint({ "n-mint" }));
    laptop.controller.Tick();
    REQUIRE(laptop.controller.Mode() == NodeMode::Pending);

    SECTION("it refused the asker")
    {
        laptop.enroll.Script({ Testing::ScriptedAnswer {
            UnsignedRefusal(), Testing::AdmissionSigning { .signer = "n-mint", .clusterId = "c-mint" } } });
        laptop.controller.Tick();
        REQUIRE(laptop.controller.Record().rejectedBy.has_value());
    }
    SECTION("it never answered")
    {
        laptop.enroll.ScriptForever(Unreachable("mint:6674"));
        laptop.controller.Tick();
        laptop.clock.advance(PendingGiveUpAfter + std::chrono::seconds { 1 });
        laptop.controller.Tick();
    }
    REQUIRE(laptop.controller.Mode() == NodeMode::Solitary);
    REQUIRE(laptop.controller.AskedJoins().size() == 1); // the memo outlives the ask
    CHECK_FALSE(laptop.controller.AskedJoins()[0].admitted);

    Machine office { "n-office", Minted("c-office", 200) };
    OfficeSeesMintListingLaptop(office, AnnouncedBy(laptop));
    CHECK(office.admin.Proposed().empty());
}

TEST_CASE("A fleet that admitted a machine here and then dissolved itself away is never yielded to",
          "[node][formation][controller][split][security]")
{
    // The variant filtering on `admitted` cannot close: c-mint ADMITS the laptop on first use, then
    // has its own leader replicate a dissolve into the office, so the laptop arrives in the office
    // carrying an admitted memo under c-mint's key. From the laptop's side that is indistinguishable
    // from a real split, so it moves nothing here either.
    Machine laptop { "n-laptop", Minted("c-laptop", 500) };
    laptop.controller.OnFleetProven(Mint({ "n-mint" }));
    laptop.controller.Tick();
    REQUIRE(laptop.controller.Mode() == NodeMode::Pending);
    laptop.enroll.Script({ AdmittedBy("n-mint", "c-mint", Testing::RosterWith("n-mint", "n-laptop")) });
    laptop.controller.Tick();
    REQUIRE(laptop.controller.Mode() == NodeMode::Learner);

    auto mint = ClusterState {};
    mint.members.push_back(Member("n-mint", "mint:6680", MemberSeat::Voter, Testing::TestKeyPair("n-mint").PublicKey()));
    mint.members.push_back(Member("n-laptop", "", MemberSeat::Learner, Testing::TestKeyPair("n-laptop").PublicKey()));
    mint.dissolveOrder = Cluster::DissolveOrder { .clusterId = "c-office",
                                                  .provenKey = Testing::TestKeyPair("n-office").PublicKey(),
                                                  .leaderNodeEndpoint = "office:6674",
                                                  .createdAtUnixSeconds = 200,
                                                  .leaderKey = Testing::TestKeyPair("n-office").PublicKey() };
    laptop.controller.OnClusterState(mint, "c-mint", "n-mint", "mint:6674");
    REQUIRE(laptop.controller.Mode() == NodeMode::Pending); // on its way to the office
    auto const memos = laptop.controller.AskedJoins();
    REQUIRE(std::ranges::any_of(memos,
                                [](Cluster::AskedJoin const& memo) { return memo.clusterId == "c-mint" && memo.admitted; }));

    Machine office { "n-office", Minted("c-office", 200) };
    OfficeSeesMintListingLaptop(office, AnnouncedBy(laptop));
    CHECK(office.admin.Proposed().empty());
}

TEST_CASE("Every member leaves for the survivor its fleet's order names, in a new cluster of its own",
          "[node][formation][controller][split]")
{
    SECTION("a learner of the fleet")
    {
        Machine laptop { "n-laptop", LearnerIn("c-laptop", "c-office") };
        laptop.controller.OnClusterState(OfficeDissolvingIntoHome(), "c-office", "n-office", "office:6674");

        CHECK(laptop.controller.Mode() == NodeMode::Pending);
        CHECK(laptop.reform.Requests() == 1);
        REQUIRE(laptop.store.Saves().size() == 1); // ONE record: nothing moves on the disk until the reform
        auto const& left = laptop.store.Saves()[0];
        CHECK(left.archivePending == std::optional<std::string> { "c-office" });
        CHECK_FALSE(left.fleet.has_value());
        CHECK(left.own.clusterId != "c-office");
        CHECK(left.own.clusterId != "c-laptop"); // a NEW cluster, not the one it once founded
        REQUIRE(left.joining.has_value());
        CHECK(Unwrap(left.joining).summary.clusterId == "c-home");
        CHECK(Unwrap(left.joining).summary.leaderNodeEndpoint == "home:6674");
        CHECK(Unwrap(left.joining).summary.nodeId.empty()); // no speaker: its leader is proved before it is asked
        CHECK(Unwrap(left.joining).provenKey == Testing::TestKeyPair("n-laptop").PublicKey());
        CHECK(
            std::ranges::any_of(left.askedJoins, [](Cluster::AskedJoin const& memo) { return memo.clusterId == "c-home"; }));
    }
    SECTION("its founder, a voter, which never reopens the cluster it left")
    {
        Machine office { "n-office", Minted("c-office", 200) };
        Lead(office);
        office.controller.OnClusterState(OfficeDissolvingIntoHome(), "c-office", "n-office", "office:6674");

        CHECK(office.controller.Mode() == NodeMode::Pending);
        auto const record = office.controller.Record();
        CHECK(record.archivePending == std::optional<std::string> { "c-office" });
        CHECK(record.own.clusterId != "c-office"); // its store is archived, and its id never serves again
        CHECK(Cluster::CurrentClusterId(record) != "c-office");
    }
}

TEST_CASE("A member that restarts mid-leave proves the survivor's leader, then is admitted there",
          "[node][formation][controller][split]")
{
    // The record the leave wrote is what a restart starts from: the start finishes the archive
    // (`ResumeFormation`, main's), and the node polls the survivor as a first join would.
    auto left = Cluster::FormationRecord {};
    {
        Machine laptop { "n-laptop", LearnerIn("c-laptop", "c-office") };
        laptop.controller.OnClusterState(OfficeDissolvingIntoHome(), "c-office", "n-office", "office:6674");
        REQUIRE(laptop.controller.Mode() == NodeMode::Pending);
        left = laptop.controller.Record();
    }

    Machine restarted { "n-laptop", left };
    auto home = FleetSummary { .clusterId = "c-home",
                               .state = FleetState::Established,
                               .createdAtUnixSeconds = 100,
                               .leaderId = "n-home",
                               .leaderNodeEndpoint = "home:6674",
                               .nodeId = "n-home",
                               .raftEndpoint = "home:6680" };
    restarted.probe.AnswerSummary("home:6674", Testing::ProvenBy(home, "n-home"));
    restarted.enroll.Script({ AdmittedBy("n-home", "c-home", Testing::RosterWith("n-home", "n-laptop")) });

    restarted.controller.Tick(); // proves who answers at home:6674
    CHECK(restarted.probe.AskedSummaries() == std::vector<std::string> { "home:6674" });
    CHECK(restarted.enroll.Asked().empty());
    restarted.controller.Tick(); // and asks it
    CHECK(restarted.controller.Mode() == NodeMode::Learner);
    CHECK(Unwrap(restarted.controller.Record().fleet).clusterId == "c-home");
}

TEST_CASE("An order naming the fleet a member is in moves nothing", "[node][formation][controller][split]")
{
    auto state = OfficeWithLaptop();
    state.dissolveOrder = Cluster::DissolveOrder { .clusterId = "c-office",
                                                   .provenKey = Testing::TestKeyPair("n-laptop").PublicKey(),
                                                   .leaderNodeEndpoint = "office:6674",
                                                   .createdAtUnixSeconds = 100 };
    Machine laptop { "n-laptop", LearnerIn("c-laptop", "c-office") };
    laptop.controller.OnClusterState(state, "c-office", "n-office", "office:6674");
    CHECK(laptop.controller.Mode() == NodeMode::Learner);
    CHECK(laptop.store.Saves().empty());
    CHECK(laptop.reform.Requests() == 0);
}

namespace
{
/// What the laptop says once it has left `c-office` for `c-home` on the dissolve: pending, in a
/// cluster minted for the wait, and pointing at the survivor's leader.
/// @return Its summary, as its beacon would carry it.
[[nodiscard]] FleetSummary LeaverPointer()
{
    Machine leaver { "n-laptop", LearnerIn("c-laptop", "c-office") };
    leaver.controller.OnClusterState(OfficeDissolvingIntoHome(), "c-office", "n-office", "office:6674");
    REQUIRE(leaver.controller.Mode() == NodeMode::Pending);
    return leaver.controller.Current();
}

/// `c-home` as its leader proves it, created at 100.
/// @return The summary.
[[nodiscard]] FleetSummary HomeAsItsLeaderSays()
{
    return FleetSummary { .clusterId = "c-home",
                          .state = FleetState::Established,
                          .createdAtUnixSeconds = 100,
                          .leaderId = "n-home",
                          .leaderNodeEndpoint = "home:6674",
                          .nodeId = "n-home",
                          .raftEndpoint = "home:6680" };
}
} // namespace

TEST_CASE("A node meeting a machine on its way to a survivor follows it there, never into the cluster being left",
          "[node][formation][controller][split][pointer]")
{
    // The laptop lost a split and is leaving for c-home: pending, in a cluster minted for the wait,
    // whose summary POINTS at the survivor. A fresh machine younger than that cluster would have
    // yielded to it by the tiebreak and been left in a cluster nobody runs; it asks the fleet the
    // pointer names instead, and joins that.
    auto const pointer = LeaverPointer();
    REQUIRE(pointer.state == FleetState::Pending);
    CHECK(pointer.clusterId != "c-home");
    CHECK(pointer.pointsAt.leaderNodeEndpoint == "home:6674"); // the survivor's leader, never its own
    CHECK(pointer.leaderNodeEndpoint.empty());                 // and no leader a reader could dial
    CHECK(pointer.pointsAt.leaderKey == std::optional { Testing::TestKeyPair("n-home").PublicKey() });

    Machine desk { "n-desk", Minted("c-desk", pointer.createdAtUnixSeconds + 1) };
    desk.probe.AnswerSummary("home:6674", Testing::ProvenBy(HomeAsItsLeaderSays(), "n-home"));
    desk.enroll.Script({ AdmittedBy("n-home", "c-home", Testing::RosterWith("n-home", "n-desk")) });
    desk.controller.OnFleetProven(Testing::ProvenBeacon(pointer));

    desk.controller.Tick(); // follows the pointer: the survivor is asked, and nobody yielded to
    CHECK(desk.probe.AskedSummaries() == std::vector<std::string> { "home:6674" });
    CHECK(desk.store.Saves().empty());
    CHECK(desk.controller.Mode() == NodeMode::Solitary);

    desk.controller.Tick(); // decides on what the survivor proved
    REQUIRE(desk.controller.Mode() == NodeMode::Pending);
    REQUIRE(desk.controller.Record().joining.has_value());
    CHECK(Unwrap(desk.controller.Record().joining).summary.clusterId == "c-home");
    CHECK(desk.controller.Current().pointsAt.leaderNodeEndpoint == "home:6674"); // and points there itself

    desk.controller.Tick(); // its leader spoke for it, so the key is known there: asked at once
    REQUIRE(desk.enroll.Asked().size() == 1);
    CHECK(desk.enroll.Asked()[0].endpoint == "home:6674");
    CHECK(desk.controller.Mode() == NodeMode::Learner);
    CHECK(Unwrap(desk.controller.Record().fleet).clusterId == "c-home");
}

TEST_CASE("A pointer is never a fleet to join: a target nobody proves is yielded to by nobody",
          "[node][formation][controller][split][pointer]")
{
    auto const pointer = LeaverPointer();
    Machine desk { "n-desk", Minted("c-desk", pointer.createdAtUnixSeconds + 1) };

    SECTION("nothing answers where it points")
    {
        desk.controller.OnFleetProven(Testing::ProvenBeacon(pointer));
        desk.controller.Tick();
        CHECK(desk.probe.AskedSummaries() == std::vector<std::string> { "home:6674" });
        CHECK(Logged(desk.logger, "which proved nothing"));
    }
    SECTION("what answers there proves a key other than the one the pointer names")
    {
        // The pointer is a HINT: it named n-home's key, and a key it names is one the answer must be
        // signed by -- never one taken as proven. c-home spoken by n-evil there is not followed.
        desk.probe.AnswerSummary("home:6674", Testing::ProvenBy(HomeAsItsLeaderSays(), "n-evil"));
        desk.controller.OnFleetProven(Testing::ProvenBeacon(pointer));
        desk.controller.Tick();
        CHECK(desk.probe.AskedSummaries() == std::vector<std::string> { "home:6674" });
        CHECK(Logged(desk.logger, "not followed"));
    }
    SECTION("what answers there is on its way elsewhere too")
    {
        auto onward = HomeAsItsLeaderSays();
        onward.state = FleetState::Pending;
        onward.leaderId.clear();
        onward.leaderNodeEndpoint.clear();
        onward.pointsAt = CompileCacheWire::JoinPointer { .leaderId = {}, .leaderNodeEndpoint = "elsewhere:6674" };
        desk.probe.AnswerSummary("home:6674", Testing::ProvenBy(onward, "n-home"));
        desk.controller.OnFleetProven(Testing::ProvenBeacon(pointer));
        desk.controller.Tick();
        CHECK(desk.probe.AskedSummaries() == std::vector<std::string> { "home:6674" });
        CHECK(Logged(desk.logger, "a pointer is followed one step"));
    }

    // Proven again on the next beat: the pointer is not asked again within the interval, and the
    // pending machine, whose cluster the tiebreak would have the desk yield to, is still never asked.
    desk.controller.OnFleetProven(Testing::ProvenBeacon(pointer));
    desk.controller.Tick();
    CHECK(desk.probe.AskedSummaries().size() == 1);
    CHECK(desk.store.Saves().empty());
    CHECK(desk.enroll.Asked().empty());
    CHECK(desk.controller.Mode() == NodeMode::Solitary);
}

namespace
{
/// A pending machine on its way somewhere, pointing at @p endpoint: what any beacon may claim.
/// @param clusterId Its own cluster.
/// @param endpoint Where it points.
/// @return The proven fleet, as a beacon proves it.
[[nodiscard]] Cluster::ProvenFleet PointingAt(std::string const& clusterId, std::string const& endpoint)
{
    auto pointer = LeaverPointer();
    pointer.clusterId = clusterId;
    pointer.pointsAt.leaderNodeEndpoint = endpoint;
    pointer.pointsAt.leaderKey.reset();
    return Testing::ProvenBeacon(pointer);
}
} // namespace

TEST_CASE("A node follows at most one pointer an interval whatever endpoint each names",
          "[node][formation][controller][split][pointer][security]")
{
    // Every pending beacon names an endpoint its sender chose. Bounded per endpoint, one beacon a beat
    // pointing somewhere new would have this node dial a chosen host:port every beat; bounded for the
    // node, it dials one an interval.
    Machine desk { "n-desk", Minted("c-desk", 5'000'000'000) };
    desk.controller.OnFleetProven(PointingAt("c-p1", "a:6674"));
    desk.controller.Tick();
    CHECK(desk.probe.AskedSummaries() == std::vector<std::string> { "a:6674" });

    desk.controller.OnFleetProven(PointingAt("c-p2", "b:6674"));
    desk.controller.Tick();
    CHECK(desk.probe.AskedSummaries() == std::vector<std::string> { "a:6674" }); // a NEW endpoint, all the same

    desk.clock.advance(SeedProbeInterval);
    desk.controller.OnFleetProven(PointingAt("c-p2", "b:6674"));
    desk.controller.Tick();
    CHECK(desk.probe.AskedSummaries() == std::vector<std::string> { "a:6674", "b:6674" });
}

TEST_CASE("A due seed probe keeps its turn however many pointers arrive", "[node][formation][controller][split][pointer]")
{
    // A pointer that pre-empted the probe would let a pending beacon every beat keep this node from
    // ever asking its own seeds -- the SRV record, the remembered fleet -- again.
    Machine desk { "n-desk", Minted("c-desk", 5'000'000'000), { "seed:6674" } };
    desk.controller.Tick(); // the typed seed, asked once before anything is decided
    REQUIRE(desk.probe.Asked().size() == 1);

    desk.controller.OnFleetProven(PointingAt("c-p1", "a:6674"));
    desk.controller.Tick(); // the interval probe is due: it is asked, and the pointer waits
    CHECK(desk.probe.Asked().size() == 2);
    CHECK(desk.probe.AskedSummaries().empty());

    desk.controller.OnFleetProven(PointingAt("c-p1", "a:6674"));
    desk.controller.Tick(); // nothing better is due: the pointer is followed
    CHECK(desk.probe.Asked().size() == 2);
    CHECK(desk.probe.AskedSummaries() == std::vector<std::string> { "a:6674" });
}

TEST_CASE("A redirect to an endpoint proving a key the chain does not reach is never asked, whatever cluster it claims",
          "[node][formation][controller][enroll][chain]")
{
    // The redirect names desk:6674, and whoever answers there proves a FLEET-SUMMARY of c-office -- the
    // right cluster id -- under n-evil's key. The office, whose key proved the fleet, says n-desk
    // leads: n-evil is not a key the chain from the office reaches, so desk is never polled, and an
    // admission n-evil signs has nothing to be believed by. Trusting a fresh probe, this was a join
    // into a fleet nobody here had proved.
    Machine laptop { "n-laptop", Pending("c-laptop", 500, "c-office", "office:6674") };
    auto desk = OfficeSummary("c-office", "desk:6674");
    desk.leaderId = "n-evil";
    desk.nodeId = "n-evil";
    laptop.probe.AnswerSummary("office:6674", OfficeNamingLeader("n-desk"));
    laptop.probe.AnswerSummary("desk:6674", Testing::ProvenBy(desk, "n-evil"));
    laptop.enroll.Script({ Redirect("desk:6674") });
    laptop.enroll.ScriptForever(AdmittedBy("n-evil", "c-office", OfficeRosterWith("n-laptop")));
    for ([[maybe_unused]] auto const beat: std::views::iota(0, 3))
        laptop.controller.Tick();

    // desk is never polled; the office is, again, under its own key -- which n-evil's admission is not.
    CHECK(laptop.enroll.AskedEndpoints() == std::vector<std::string> { "office:6674", "office:6674" });
    CHECK(laptop.controller.Mode() == NodeMode::Pending);
    CHECK(laptop.store.Saves().empty());
    CHECK(Logged(laptop.logger, "reached from the key that proved it"));
}

TEST_CASE("A leader the decided summary only names is held to the key that summary states for it",
          "[node][formation][controller][enroll][chain]")
{
    // n-office spoke, and said n-desk leads, under n-desk's key -- inside what n-office signed. So desk
    // must prove exactly that key before it is asked; a desk proving another key for c-office is not.
    struct Row
    {
        std::string_view name; ///< The case.
        std::string signer;    ///< Who proves c-office at desk:6674.
        bool admitted;         ///< Whether the join goes through.
    };
    for (auto const& row: { Row { .name = "desk proves the stated key", .signer = "n-desk", .admitted = true },
                            Row { .name = "desk proves another key", .signer = "n-evil", .admitted = false } })
    {
        INFO(row.name);
        auto record = Pending("c-laptop", 500, "c-office", "desk:6674");
        REQUIRE(record.joining.has_value());
        auto joining = Unwrap(record.joining);
        joining.summary.leaderId = "n-desk";
        joining.summary.leaderKey = Testing::TestKeyPair("n-desk").PublicKey();
        record.joining = std::move(joining);
        Machine laptop { "n-laptop", record };
        auto desk = OfficeSummary("c-office", "desk:6674");
        desk.leaderId = row.signer;
        desk.nodeId = row.signer;
        laptop.probe.AnswerSummary("desk:6674", Testing::ProvenBy(desk, row.signer));
        laptop.enroll.ScriptForever(AdmittedBy(row.signer, "c-office", OfficeRosterWith("n-laptop")));

        laptop.controller.Tick(); // desk proves a key, or fails to
        laptop.controller.Tick(); // and is asked only if it proved the stated one
        REQUIRE_FALSE(laptop.probe.AskedSummaries().empty());
        CHECK(laptop.probe.AskedSummaries().front() == "desk:6674");
        CHECK((laptop.controller.Mode() == NodeMode::Learner) == row.admitted);
        CHECK(laptop.enroll.AskedEndpoints().empty() == !row.admitted);
    }
}

TEST_CASE("A refusal nobody the chain reaches signed is no answer, and never sends the node away",
          "[node][formation][controller][enroll][chain]")
{
    // A refusal sends a joiner back to solitary and keeps it from that fleet for an hour, so it is the
    // fleet's word or none: unsigned, or signed by a key this node never proved, it is refused by name,
    // counted, and read as NO answer -- asked again on the next beat, never recorded as a rejection.
    struct Row
    {
        std::string_view name;               ///< The case.
        Testing::ScriptedAnswer answer;      ///< What answers the poll.
        Cluster::AdmissionSignature verdict; ///< What the node finds it to be.
    };
    auto const rows = std::array {
        Row { .name = "unsigned", .answer = UnsignedRefusal(), .verdict = Cluster::AdmissionSignature::Unsigned },
        Row { .name = "signed by n-evil",
              .answer =
                  Testing::ScriptedAnswer { UnsignedRefusal(),
                                            Testing::AdmissionSigning { .signer = "n-evil", .clusterId = "c-office" } },
              .verdict = Cluster::AdmissionSignature::Unproven },
    };
    for (auto const& row: rows)
    {
        INFO(row.name);
        Machine laptop { "n-laptop", Pending("c-laptop", 500, "c-office", "office:6674") };
        laptop.enroll.Script({ row.answer });
        laptop.controller.Tick();
        CHECK(laptop.controller.Mode() == NodeMode::Pending);
        CHECK_FALSE(laptop.controller.Record().rejectedBy.has_value());
        CHECK(laptop.store.Saves().empty());
        CHECK(laptop.metrics.Read(IMetricsSink::Counter::FormationAdmissionsUnverified) == 1);
        CHECK(Logged(laptop.logger, Cluster::WordsFor(row.verdict)));

        // The control: the office's own refusal, signed over this request, is one.
        laptop.enroll.Script({ Refused() });
        laptop.controller.Tick();
        CHECK(laptop.controller.Mode() == NodeMode::Solitary);
        CHECK(laptop.controller.Record().rejectedBy.has_value());
    }
}

TEST_CASE("A wait nobody signed keeps nobody waiting, and the join is given up as if nothing answered",
          "[node][formation][controller][enroll][chain]")
{
    // Any "not yet" nobody signed would otherwise keep a node polling a join that is dead: each one
    // reset the give-up. Whoever answers the poll's connection can say any of them every beat -- an
    // unsigned "not yet", a full list, a NotLeader naming nobody, a redirect -- so each counts towards
    // giving up like an endpoint that does not answer; only the fleet's own SIGNED "not yet" keeps the
    // node waiting past the same bound.
    struct Row
    {
        std::string_view name;          ///< The case.
        Testing::ScriptedAnswer answer; ///< What answers every poll.
        bool abandoned;                 ///< Whether the join is given up.
        std::uint64_t unverified;       ///< How many answers claimed a signature that did not verify.
    };
    auto const rows = std::array {
        Row { .name = "an unsigned wait", .answer = UnsignedWaiting(), .abandoned = true, .unverified = 2 },
        Row { .name = "a full list, on the wire",
              .answer = Reading(EnrollProgress::Closed, "the enrollment list is full"),
              .abandoned = true,
              .unverified = 0 },
        Row { .name = "NotLeader naming nobody, on the wire",
              .answer = Reading(EnrollProgress::Closed, "no leader: try again"),
              .abandoned = true,
              .unverified = 0 },
        Row { .name = "a redirect", .answer = Redirect("elsewhere:6674"), .abandoned = true, .unverified = 0 },
        Row { .name = "the office's own", .answer = Waiting(), .abandoned = false, .unverified = 0 },
    };
    for (auto const& row: rows)
    {
        INFO(row.name);
        Machine laptop { "n-laptop", Pending("c-laptop", 500, "c-office", "office:6674") };
        laptop.enroll.ScriptForever(row.answer);
        laptop.controller.Tick();
        laptop.clock.advance(PendingGiveUpAfter + std::chrono::seconds { 1 });
        laptop.controller.Tick();
        CHECK((laptop.controller.Mode() == NodeMode::Solitary) == row.abandoned);
        CHECK((laptop.metrics.Read(IMetricsSink::Counter::FormationJoinsAbandoned) == 1) == row.abandoned);
        CHECK(laptop.metrics.Read(IMetricsSink::Counter::FormationAdmissionsUnverified) == row.unverified);
    }
}

TEST_CASE("A fleet's leader proposes no dissolve into a survivor whose leader no proven key vouches for",
          "[node][formation][controller][split][chain]")
{
    // Every member goes to the survivor's leader next and holds its answers to ONE key, reached from
    // the key that proved the survivor. The desk speaks for c-home but names no key for n-home, so no
    // key is reached, and the fleet is not sent to a leader nobody could hold to anything.
    Machine office { "n-office", Minted("c-office", 200) };
    Lead(office);
    auto keyless = Home("n-desk", 100).Summary();
    keyless.leaderKey.reset();
    office.controller.OnFleetProven(Testing::ProvenBeacon(keyless));
    office.controller.Tick();
    CHECK(office.admin.Proposed().empty());

    // The control: the same summary naming n-home's key is proposed, with that key.
    office.controller.OnFleetProven(Home("n-desk", 100));
    office.controller.Tick();
    REQUIRE(office.admin.Proposed().size() == 1);
    CHECK(office.admin.Proposed()[0].leaderKey == std::optional { Testing::TestKeyPair("n-home").PublicKey() });
}

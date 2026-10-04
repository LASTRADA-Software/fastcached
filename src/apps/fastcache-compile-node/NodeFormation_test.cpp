// SPDX-License-Identifier: Apache-2.0
#include "ConsensusTier.hpp"
#include "NodeConditions.hpp"
#include "NodeConfig.hpp"
#include "NodeDefaults.hpp"
#include "NodeFormation.hpp"
#include "NodeSurfaces.hpp"

#include <FastCache/Cluster/FleetEndpoints.hpp>
#include <FastCache/Cluster/FormationRecord.hpp>
#include <FastCache/Cluster/NodeMode.hpp>
#include <FastCache/Cluster/Roster.hpp>
#include <FastCache/Core/HostPort.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Testing::Unwrap;

namespace
{

/// This node's id in every roster below: a learner of the office fleet.
constexpr auto ThisNode = "this-pc";

/// The fleet a learner was admitted to: one voter, `office`, and this node as a learner with no
/// endpoint (a learner dials in).
/// @return The roster.
[[nodiscard]] Cluster::Roster OfficeRoster()
{
    return Cluster::Roster { .members = { Cluster::RosterMember { .id = "office",
                                                                  .raftEndpoint = "office:6680",
                                                                  .seat = Cluster::MemberSeat::Voter,
                                                                  .publicKey = Testing::TestKeyPair("office").PublicKey(),
                                                                  .schedulerEndpoint = {} },
                                          Cluster::RosterMember { .id = ThisNode,
                                                                  .raftEndpoint = {},
                                                                  .seat = Cluster::MemberSeat::Learner,
                                                                  .publicKey = Testing::TestKeyPair(ThisNode).PublicKey(),
                                                                  .schedulerEndpoint = {} } },
                             .revoked = {} };
}

/// A formation record in @p mode, with the office fleet's roster wherever the mode joined one.
/// @param mode The mode.
/// @return The record.
[[nodiscard]] Cluster::FormationRecord RecordIn(Cluster::NodeMode mode)
{
    auto record = Cluster::FormationRecord { .mode = mode,
                                             .own = { .clusterId = "own-c", .createdAtUnixSeconds = 100 },
                                             .joining = std::nullopt,
                                             .fleet = std::nullopt,
                                             .archivePending = std::nullopt,
                                             .rejectedBy = std::nullopt,
                                             .askedJoins = {} };
    if (mode == Cluster::NodeMode::Learner)
        record.fleet = Cluster::FleetMembership { .clusterId = "fleet-c",
                                                  .roster = Cluster::EncodeRoster(OfficeRoster()),
                                                  .createdAtUnixSeconds = 0,
                                                  .admittedBy = Testing::TestKeyPair("office").PublicKey() };
    return record;
}

/// A configuration of this node, shaped by @p record.
/// @param record The formation record.
/// @param remembered The fleet endpoints it last knew.
/// @return The configuration.
[[nodiscard]] NodeConfig FormedBy(Cluster::FormationRecord const& record, Cluster::FleetEndpoints const& remembered = {})
{
    auto cfg = NodeConfig {};
    cfg.nodeId = ThisNode;
    auto const applied = ApplyFormation(cfg, record, remembered);
    REQUIRE(applied.has_value());
    return cfg;
}

/// This machine's name as a RHEL box or a container that never set one names it.
/// @return The host names, withheld.
[[nodiscard]] NodeHostNames Withheld()
{
    return NodeHostNames { .fqdn = {}, .dnsSuffix = {}, .withheld = "localhost" };
}

} // namespace

TEST_CASE("Each mode opens the Raft port its row says and runs the consensus it says", "[node][formation][mode]")
{
    using enum Cluster::NodeMode;
    struct Row
    {
        Cluster::NodeMode mode;
        bool consensus;
        bool raftPort;
        bool scheduler;
        bool enrollment;
    };
    for (auto const row:
         { Row { .mode = Solitary, .consensus = true, .raftPort = true, .scheduler = true, .enrollment = true },
           Row { .mode = Pending, .consensus = true, .raftPort = true, .scheduler = true, .enrollment = true },
           Row { .mode = Learner, .consensus = true, .raftPort = false, .scheduler = false, .enrollment = false },
           Row { .mode = Voter, .consensus = true, .raftPort = true, .scheduler = true, .enrollment = true } })
    {
        INFO(Cluster::NodeModeRowFor(row.mode).name);
        auto const cfg = FormedBy(RecordIn(row.mode));
        CHECK(RunsConsensus(cfg) == row.consensus);
        CHECK(RowFor(NodeSurface::Raft).Resolve(cfg).empty() == !row.raftPort);
        CHECK(ServesScheduler(cfg) == row.scheduler);
        // Asked as `main` asks it: of the configuration, and of whether the scheduler was BUILT.
        CHECK(ServesEnrollment(cfg, ServesScheduler(cfg)) == row.enrollment);
        // A scheduler tier that failed to start serves no window, whatever the mode says.
        CHECK_FALSE(ServesEnrollment(cfg, false));
    }
}

TEST_CASE("A configuration no record shaped runs no consensus and opens no raft port", "[node][formation][mode]")
{
    auto const cfg = NodeConfig {};
    CHECK_FALSE(RunsConsensus(cfg));
    CHECK_FALSE(ServesScheduler(cfg));
    CHECK_FALSE(ServesEnrollment(cfg, true));
    CHECK(RowFor(NodeSurface::Raft).Resolve(cfg).empty());
    CHECK(SchedulersOf(cfg, AsConfigured).empty());
    CHECK(BootstrapMembersOf(cfg).empty());
}

TEST_CASE("An empty --listen-raft closes consensus for a mode that listens and not for one that dials in",
          "[node][formation][mode]")
{
    // The one flag that still closes the port until the formation retires the flags that carried
    // a cluster's shape: a fixture's worker running no consensus says so with it. A learner opens
    // no port whatever it says, and still runs its fleet's consensus.
    for (auto const& mode: Cluster::NodeModeTable)
    {
        INFO(mode.name);
        auto cfg = FormedBy(RecordIn(mode.mode));
        cfg.raftListen.clear();
        CHECK(RunsConsensus(cfg) == !ModeOpensRaftPort(mode.mode));
        CHECK(RowFor(NodeSurface::Raft).Resolve(cfg).empty());
    }
}

TEST_CASE("A pending node keeps serving exactly what it served while solitary", "[node][formation][mode]")
{
    auto const solitary = FormedBy(RecordIn(Cluster::NodeMode::Solitary));
    auto const pending = FormedBy(RecordIn(Cluster::NodeMode::Pending));
    CHECK(ServesScheduler(pending) == ServesScheduler(solitary));
    CHECK(SchedulersOf(pending, AsConfigured) == SchedulersOf(solitary, AsConfigured));
    CHECK(SchedulersOf(pending, AsConfigured) == std::vector<std::string> { "127.0.0.1:6674" });
    CHECK(BootstrapMembersOf(pending).size() == 1);
    CHECK(Unwrap(pending.formation).clusterId == Unwrap(solitary.formation).clusterId);
    CHECK(pending.clusterId == "own-c");
}

TEST_CASE("A node serving no scheduler registers at the voters its applied state records, else where its formation says",
          "[node][formation][announce]")
{
    // T26's carry: the formation record remembers the voters' endpoints as the approval carried them,
    // and a voter that moved announces its new one, proven, which the leader records. So the applied
    // state answers, REPLACING the remembered list rather than joining it -- a moved voter's old
    // endpoint is dead -- and the remembered list answers only until the state names a voter.
    auto const self = FormedBy(
        RecordIn(Cluster::NodeMode::Learner),
        Cluster::FleetEndpoints { .clusterId = "fleet-c",
                                  .voters = { Cluster::FleetEndpoint {
                                      .id = "office", .raftEndpoint = "office:6680", .nodeEndpoint = "office:6674" } } });
    REQUIRE_FALSE(ServesScheduler(self));
    auto const remembered = SchedulersOf(self, AsConfigured);
    REQUIRE(remembered == std::vector<std::string> { "office:6674" });
    AppliedSchedulers schedulers { self, AsConfigured };
    CHECK(schedulers.Current() == remembered);

    auto const member = [](std::string id, std::string endpoint, Cluster::MemberSeat seat) {
        return Cluster::ClusterMember { .id = std::move(id),
                                        .raftEndpoint = {},
                                        .schedulerEndpoint = std::move(endpoint),
                                        .schedulerEndpointHistory = Cluster::SchedulerEndpointHistory::Announced,
                                        .seat = seat,
                                        .publicKey = {} };
    };
    auto state = Cluster::ClusterState {};
    state.members = { member("office", "office.example:6674", Cluster::MemberSeat::Voter),
                      member("desk", "desk.example:6674", Cluster::MemberSeat::Learner),
                      member(ThisNode, "this-pc.example:6674", Cluster::MemberSeat::Voter),
                      member("annex", "", Cluster::MemberSeat::Voter),
                      member("lab", "lab.example:6674", Cluster::MemberSeat::Voter) };
    // Voters only, in the state's order: never a learner, never this node, never a voter that has
    // announced no endpoint yet.
    auto const recorded = std::vector<std::string> { "office.example:6674", "lab.example:6674" };
    CHECK(RecordedSchedulersOf(state, ThisNode) == recorded);
    schedulers.Applied(state);
    CHECK(schedulers.Current() == recorded);

    // A state naming no voter's endpoint hands the answer back to the formation.
    schedulers.Applied(Cluster::ClusterState {});
    CHECK(schedulers.Current() == remembered);

    // A node serving its own scheduler registers there, whatever any state records.
    auto const solitary = FormedBy(RecordIn(Cluster::NodeMode::Solitary));
    AppliedSchedulers own { solitary, AsConfigured };
    own.Applied(state);
    CHECK(own.Current() == SchedulersOf(solitary, AsConfigured));
}

TEST_CASE("A node serving its own scheduler registers at the port it serves, a handed-over one included",
          "[node][formation][mode][activation]")
{
    // The packaged unit hands the node surface over on 6676 while `--listen-node` defaults to 6674,
    // so registering at the configuration's port sent the node's own worker to a port it does not
    // serve -- or to whatever else holds 6674 -- and it was never sent a job.
    auto const solitary = FormedBy(RecordIn(Cluster::NodeMode::Solitary));
    REQUIRE(ServesScheduler(solitary));
    // A bare `ListenStream=6676` binds every interface, so it is dialled at loopback -- in either
    // family's spelling of the wildcard.
    CHECK(SchedulersOf(solitary, ActivatedEndpoint { .host = "0.0.0.0", .port = 6676 })
          == std::vector<std::string> { "127.0.0.1:6676" });
    CHECK(SchedulersOf(solitary, ActivatedEndpoint { .host = "::", .port = 6676 })
          == std::vector<std::string> { "127.0.0.1:6676" });
    // A unit bound to ONE address (`ListenStream=10.0.0.5:6676`) answers there alone, so that is
    // where it is dialled; loopback would reach nothing.
    CHECK(SchedulersOf(solitary, ActivatedEndpoint { .host = "10.0.0.5", .port = 6676 })
          == std::vector<std::string> { "10.0.0.5:6676" });

    // Bound by the node itself, it is the port the surface binds -- the one `--print-surfaces`
    // prints, from the same row, so the map and the registration cannot disagree.
    CHECK(SchedulersOf(solitary, AsConfigured) == std::vector<std::string> { "127.0.0.1:6674" });
    auto bound = solitary;
    bound.nodeListen = "127.0.0.1:6690";
    CHECK(SchedulersOf(bound, AsConfigured) == std::vector<std::string> { "127.0.0.1:6690" });
    CHECK(RenderSurfaces(bound).contains("127.0.0.1:6690"));

    // Dialled at the address it binds: a node bound to one address answers there alone, so
    // loopback would reach nothing, and a wildcard bind answers on loopback.
    bound.nodeListen = "10.0.0.5:6690";
    CHECK(SchedulersOf(bound, AsConfigured) == std::vector<std::string> { "10.0.0.5:6690" });
    bound.nodeListen = "0.0.0.0:6690";
    CHECK(SchedulersOf(bound, AsConfigured) == std::vector<std::string> { "127.0.0.1:6690" });

    // A learner registers with its fleet, whatever socket it was handed.
    auto const remembered = Cluster::FleetEndpoints {
        .clusterId = "fleet-c",
        .voters = { { .id = "office", .raftEndpoint = "office:6680", .nodeEndpoint = "office:6674" } }
    };
    auto const learner = FormedBy(RecordIn(Cluster::NodeMode::Learner), remembered);
    CHECK(SchedulersOf(learner, ActivatedEndpoint { .host = "0.0.0.0", .port = 6676 })
          == std::vector<std::string> { "office:6674" });
}

TEST_CASE("A node bound to its LAN address reads its own scheduler as this machine and a fleet's as remote",
          "[node][formation][mode]")
{
    // Its own scheduler is dialled at the one address it binds (`SchedulersOf`), which is not
    // loopback -- so a test of "names only this machine" alone called it remote. It is this
    // machine's own address, decided through `SameHost` against what this node binds and states.
    auto bound = FormedBy(RecordIn(Cluster::NodeMode::Solitary));
    bound.nodeListen = "10.0.0.5:6690";
    ApplyHostNames(bound, NodeHostNames { .fqdn = "this-pc.corp.example", .dnsSuffix = "corp.example", .withheld = {} });
    REQUIRE(SchedulersOf(bound, AsConfigured) == std::vector<std::string> { "10.0.0.5:6690" });
    CHECK_FALSE(SchedulerIsRemote(bound));

    // A typed --advertise names this machine too: a scheduler there is its own.
    auto advertised = FormedBy(RecordIn(Cluster::NodeMode::Learner));
    advertised.advertise = "build-7.corp.example:6674";
    REQUIRE(advertised.formation.has_value());
    if (advertised.formation.has_value())
        advertised.formation->fleetSchedulers = { "build-7.corp.example:6674" };
    REQUIRE_FALSE(ServesScheduler(advertised));
    CHECK_FALSE(SchedulerIsRemote(advertised));

    // The control: a learner bound to the same LAN address registers with its fleet's scheduler on
    // another machine, and that one IS remote.
    auto learner = FormedBy(RecordIn(Cluster::NodeMode::Learner));
    learner.nodeListen = "10.0.0.5:6690";
    REQUIRE(learner.formation.has_value());
    if (learner.formation.has_value())
        learner.formation->fleetSchedulers = { "10.0.0.9:6674" };
    REQUIRE(SchedulersOf(learner, AsConfigured) == std::vector<std::string> { "10.0.0.9:6674" });
    CHECK(SchedulerIsRemote(learner));
}

TEST_CASE("A learner starts from the approved roster and registers with the fleet's leader first", "[node][formation][mode]")
{
    auto const remembered = Cluster::FleetEndpoints {
        .clusterId = "fleet-c",
        .voters = { { .id = "office", .raftEndpoint = "office:6680", .nodeEndpoint = "office:6674" } }
    };
    auto const cfg = FormedBy(RecordIn(Cluster::NodeMode::Learner), remembered);
    CHECK(Unwrap(cfg.formation).clusterId == "fleet-c");
    CHECK_FALSE(Unwrap(cfg.formation).foundedHere);
    // The lease signature's cluster field follows the record.
    CHECK(cfg.clusterId == "fleet-c");
    CHECK(SchedulersOf(cfg, AsConfigured) == std::vector<std::string> { "office:6674" });
    auto const members = BootstrapMembersOf(cfg);
    // Never itself: its own seat is the leader's to replicate.
    CHECK(std::ranges::none_of(members, [&](auto const& m) { return m.id == cfg.nodeId; }));
    CHECK(std::ranges::contains(members, std::string { "office" }, &Cluster::MemberSpec::id));
}

TEST_CASE("Remembered endpoints of another fleet are not registered with", "[node][formation][mode]")
{
    // A fleet-endpoints file outlives the fleet it was written for; a node that joined another one
    // must not hand its worker to the old fleet's voters.
    auto const stale =
        Cluster::FleetEndpoints { .clusterId = "old-fleet",
                                  .voters = { { .id = "gone", .raftEndpoint = "gone:6680", .nodeEndpoint = "gone:6674" } } };
    auto const cfg = FormedBy(RecordIn(Cluster::NodeMode::Learner), stale);
    CHECK(SchedulersOf(cfg, AsConfigured).empty());
}

TEST_CASE("A founder voter bootstraps itself and a promoted voter does not", "[node][formation][mode]")
{
    auto const founder = FormedBy(RecordIn(Cluster::NodeMode::Voter));
    CHECK(Unwrap(founder.formation).foundedHere);
    auto const alone = BootstrapMembersOf(founder);
    REQUIRE(alone.size() == 1);
    CHECK(alone.front().id == ThisNode);

    auto promotedRecord = RecordIn(Cluster::NodeMode::Voter);
    promotedRecord.fleet = Cluster::FleetMembership { .clusterId = "fleet-c",
                                                      .roster = Cluster::EncodeRoster(OfficeRoster()),
                                                      .createdAtUnixSeconds = 0,
                                                      .admittedBy = Testing::TestKeyPair("office").PublicKey() };
    auto const promoted = FormedBy(promotedRecord);
    CHECK_FALSE(Unwrap(promoted.formation).foundedHere);
    auto const members = BootstrapMembersOf(promoted);
    CHECK_FALSE(members.empty());

    // One cluster id per FORMED cluster: every voter it promoted signs and checks under the
    // fleet's, never under the id its own solitary cluster was minted with -- or no majority of
    // them could ever endorse the same roster.
    CHECK(founder.clusterId == "own-c");
    CHECK(promoted.clusterId == "fleet-c");
    CHECK(std::ranges::none_of(members, [&](auto const& m) { return m.id == promoted.nodeId; }));
}

TEST_CASE("A record whose roster does not decode refuses rather than forming an empty fleet", "[node][formation][mode]")
{
    auto record = RecordIn(Cluster::NodeMode::Learner);
    record.fleet = Cluster::FleetMembership { .clusterId = "fleet-c",
                                              .roster = { std::byte { 0xFF }, std::byte { 0x00 } },
                                              .createdAtUnixSeconds = 0,
                                              .admittedBy = Testing::TestKeyPair("office").PublicKey() };
    auto cfg = NodeConfig {};
    auto const applied = ApplyFormation(cfg, record, {});
    REQUIRE_FALSE(applied.has_value());
    CHECK(applied.error().contains("fleet-c"));
    CHECK(applied.error().contains("roster"));
    // Nothing half-applied: a configuration the start refuses is not one anything reads.
    CHECK_FALSE(cfg.formation.has_value());
}

TEST_CASE("A machine whose name reaches only itself is confined to loopback or refused under every mode",
          "[node][formation][mode][defaults]")
{
    // Walked over the mode TABLE, so a mode added tomorrow is judged here with no edit. The
    // expectation is read off the same two columns the rule reads: a mode that opens the port
    // binds it to loopback on a name nobody else can dial -- a fleet of its own -- a learner dials
    // out and needs no name, and a mode other machines dial is refused by name.
    //
    // **The EXACT outcome, never "not that refusal"**: every other refusal also satisfies a
    // negative, and one did -- a learner was refused for naming no dial address, on every
    // machine, while this loop stayed green.
    for (auto const& mode: Cluster::NodeModeTable)
    {
        INFO(mode.name);
        auto cfg = FormedBy(RecordIn(mode.mode));
        cfg.slots = 0;
        ApplyHostNames(cfg, Withheld());

        CHECK(RunsConsensus(cfg));
        CHECK(ConsensusConfinedToThisMachine(cfg) == ModeOpensRaftPort(mode.mode));
        auto const raft = RowFor(NodeSurface::Raft).Resolve(cfg);
        CHECK(raft.empty() == !ModeOpensRaftPort(mode.mode));
        for (auto const& endpoint: raft)
            CHECK(endpoint.host == ThisMachineLoopbackHost);
        auto const expected = ModeServesConsensusToPeers(mode.mode)
                                  ? std::optional { std::string { ConsensusNameReachesOnlyThisMachineRefusal } }
                                  : std::optional<std::string> {};
        CHECK(StartupPolicyRejection(cfg) == expected);

        // The control: the same mode on a machine with a name peers dial runs consensus, is
        // refused for nothing at all, and opens the port the mode says.
        auto named = FormedBy(RecordIn(mode.mode));
        named.slots = 0;
        ApplyHostNames(named, NodeHostNames { .fqdn = "box.corp.example", .dnsSuffix = "corp.example", .withheld = {} });
        CHECK(RunsConsensus(named));
        CHECK(RowFor(NodeSurface::Raft).Resolve(named).empty() == !ModeOpensRaftPort(mode.mode));
        CHECK(StartupPolicyRejection(named) == std::optional<std::string> {});
    }
}

TEST_CASE("Consensus runs as a member of its own under every mode, and only a mode nobody dials needs no address",
          "[node][formation][mode][consensus]")
{
    // Walked over the mode TABLE. A mode whose row closes the Raft port dials the fleet's voters
    // and is dialled by nobody, so it starts with no address -- its roster entry, or a learner's
    // record built with none -- where every other mode is refused by name for naming none. Both
    // the startup table and the tier's own record are asked, because both refused a learner.
    auto const named = NodeHostNames { .fqdn = "box.corp.example", .dnsSuffix = "corp.example", .withheld = {} };
    auto const unnamed = NodeHostNames { .fqdn = {}, .dnsSuffix = {}, .withheld = {} };
    for (auto const& mode: Cluster::NodeModeTable)
    {
        INFO(mode.name);
        auto const dialsIn = !ModeOpensRaftPort(mode.mode);
        for (auto const& names: { named, unnamed })
        {
            INFO("fqdn: " << names.fqdn);
            auto cfg = FormedBy(RecordIn(mode.mode));
            cfg.slots = 0;
            ApplyHostNames(cfg, names);
            REQUIRE(RunsConsensus(cfg));
            auto const statesAnAddress = dialsIn || !names.fqdn.empty();

            CHECK(StartupPolicyRejection(cfg)
                  == (statesAnAddress ? std::optional<std::string> {}
                                      : std::optional { std::string { ConsensusNamesNoDialAddressRefusal } }));

            // From the members the formation starts with, and from none -- which is where the
            // record is built rather than found.
            for (auto const& members: { BootstrapMembersOf(cfg), std::vector<Cluster::MemberSpec> {} })
            {
                INFO("members: " << members.size());
                auto const self = ConsensusSelfMemberOf(cfg, members, Testing::TestKeyPair(ThisNode).PublicKey());
                if (!statesAnAddress)
                {
                    REQUIRE_FALSE(self.has_value());
                    CHECK(self.error() == ConsensusNamesNoDialAddressRefusal);
                    continue;
                }
                REQUIRE(self.has_value());
                CHECK(self->id == ThisNode);
                CHECK(self->raftEndpoint.empty() == dialsIn);
                CHECK(self->seat == (dialsIn ? Cluster::MemberSeat::Learner : Cluster::MemberSeat::Voter));
            }
        }
    }
}

TEST_CASE("A worker on a machine whose name reaches only itself registers at loopback with its own scheduler",
          "[node][formation][mode][defaults]")
{
    // The scheduler this node serves is on this machine, so it reaches the worker at loopback --
    // and a name every machine resolves to itself is offered to nobody else. Registering with a
    // scheduler on ANOTHER machine is the case the refusal is for, and it still refuses.
    auto worker = FormedBy(RecordIn(Cluster::NodeMode::Solitary));
    REQUIRE(ServesScheduler(worker));
    worker.toolchains = { "/usr/bin/g++" };
    ApplyHostNames(worker, Withheld());

    SECTION("its own scheduler")
    {
        CHECK(AdvertisedNameWithheld(worker));
        CHECK(AdvertisedEndpoint(worker) == "127.0.0.1:6674");
        CHECK(StartupPolicyRejection(worker) != std::optional { std::string { WorkerNameReachesOnlyThisMachineRefusal } });
        auto const detail = NameReachesOnlyThisMachineInUse(worker);
        REQUIRE(detail.has_value());
        CHECK(Unwrap(detail).contains("loopback"));
    }
    // A learner registers with its fleet's scheduler, which its record names.
    auto const learnerOf = [](NodeConfig cfg, std::vector<std::string> fleet) {
        cfg.formation->mode = Cluster::NodeMode::Learner;
        cfg.formation->foundedHere = false;
        cfg.formation->fleetSchedulers = std::move(fleet);
        REQUIRE_FALSE(ServesScheduler(cfg));
        return cfg;
    };
    SECTION("a scheduler on another machine")
    {
        worker = learnerOf(worker, { "sched.corp.example:6675" });
        CHECK(AdvertisedEndpoint(worker).empty());
        CHECK(StartupPolicyRejection(worker) == std::optional { std::string { WorkerNameReachesOnlyThisMachineRefusal } });
    }
    SECTION("any one of several schedulers on another machine")
    {
        worker = learnerOf(worker, { "127.0.0.1:6674", "sched.corp.example:6675" });
        CHECK(StartupPolicyRejection(worker) == std::optional { std::string { WorkerNameReachesOnlyThisMachineRefusal } });
    }
}

TEST_CASE(
    "A machine whose name reaches only itself runs a fleet of its own with consensus and scheduler and worker on loopback",
    "[node][formation][mode][defaults]")
{
    // Neither standing down nor refused: a node needs no peer to be a fleet of one, so its consensus
    // binds loopback, its scheduler signs grants, and its own worker registers there and checks them
    // against the state it applies. What it cannot do is become more than one machine -- discovery,
    // enrollment and a FLEET-SUMMARY are all closed -- and the condition says so, with the remedy.
    //
    // The PENDING row is the review's path (I-2): a node that asked a fleet to take it, restarting on a
    // name that reaches only itself. It must not register at the leader it asked holding no roster; it
    // stays its own cluster, serving its own scheduler, and its worker registers there.
    auto pending = RecordIn(Cluster::NodeMode::Pending);
    pending.joining =
        Cluster::JoinTarget { .summary = CompileCacheWire::FleetSummary { .clusterId = "fleet-c",
                                                                          .leaderNodeEndpoint = "office.corp.example:6674" },
                              .provenKey = Testing::TestKeyPair("office").PublicKey(),
                              .askedAtUnixSeconds = 90 };
    for (auto const& record: { RecordIn(Cluster::NodeMode::Solitary), pending })
    {
        INFO(Cluster::NodeModeRowFor(record.mode).name);
        auto cfg = FormedBy(record);
        cfg.toolchains = { "/usr/bin/g++" };
        ApplyHostNames(cfg, Withheld());

        INFO("refusal: " << StartupPolicyRejection(cfg).value_or("<none>"));
        CHECK(StartupPolicyRejection(cfg) == std::optional<std::string> {});
        CHECK(ConsensusConfinedToThisMachine(cfg));
        CHECK(RunsConsensus(cfg));
        CHECK(ServesScheduler(cfg));
        CHECK(SchedulersOf(cfg, AsConfigured) == std::vector<std::string> { "127.0.0.1:6674" });
        CHECK(AdvertisedEndpoint(cfg) == "127.0.0.1:6674");
        CHECK_FALSE(SchedulerIsRemote(cfg));
        auto const dial = ConsensusDialAddressOf(cfg);
        REQUIRE(dial.has_value());
        CHECK(HostOfEndpoint(Unwrap(dial)) == ThisMachineLoopbackHost);
        CHECK(RowFor(NodeSurface::Discovery).Resolve(cfg).empty());
        CHECK_FALSE(ServesEnrollment(cfg, true));

        auto const detail = NameReachesOnlyThisMachineInUse(cfg);
        REQUIRE(detail.has_value());
        CHECK(Unwrap(detail).contains("can neither form nor join a fleet"));
        NodeConditions conditions;
        EvaluateHostNameCondition(conditions, cfg);
        CHECK(conditions.StateOf(NodeCondition::HostNameReachesOnlyThisMachine) == CompileCacheWire::ConditionState::Raised);

        // The control: the same record on a machine whose name peers dial changes nothing it did
        // before -- consensus at that name, discovery and enrollment open, no condition.
        auto named = FormedBy(record);
        named.toolchains = { "/usr/bin/g++" };
        ApplyHostNames(named, NodeHostNames { .fqdn = "this-pc.corp.example", .dnsSuffix = "corp.example", .withheld = {} });
        CHECK(StartupPolicyRejection(named) == std::optional<std::string> {});
        CHECK_FALSE(ConsensusConfinedToThisMachine(named));
        CHECK(RunsConsensus(named));
        CHECK(ServesScheduler(named));
        CHECK(SchedulersOf(named, AsConfigured) == std::vector<std::string> { "127.0.0.1:6674" });
        auto const namedDial = ConsensusDialAddressOf(named);
        REQUIRE(namedDial.has_value());
        CHECK(HostOfEndpoint(Unwrap(namedDial)) == "this-pc.corp.example");
        CHECK_FALSE(RowFor(NodeSurface::Discovery).Resolve(named).empty());
        CHECK(ServesEnrollment(named, true));
        CHECK_FALSE(NameReachesOnlyThisMachineInUse(named).has_value());
    }
}

TEST_CASE("A learner and a voter whose fleet records a learner pass every startup rule", "[node][formation][mode]")
{
    // The node-level half of the two go-live blockers: a learner -- which runs consensus and binds no
    // Raft port -- and a voter whose roster names a learner with no endpoint, judged by the rules a
    // start asks before any tier exists. Their tiers' starts are `ConsensusTier_test`'s.
    //
    // Neither names a scheduler: each worker registers where its record says (`SchedulersOf`), and a
    // node that serves is refused `--scheduler`, which aims one-shot verbs alone.
    auto named = [](NodeConfig cfg) {
        cfg.hostNames = NodeHostNames { .fqdn = "this-pc.corp.example", .dnsSuffix = "corp.example", .withheld = {} };
        return cfg;
    };

    auto const learner = named(FormedBy(RecordIn(Cluster::NodeMode::Learner)));
    REQUIRE(RunsConsensus(learner));
    CHECK(StartupPolicyRejection(learner).value_or("(none)") == "(none)"); // the refusal itself, when one is made

    auto voterRecord = RecordIn(Cluster::NodeMode::Voter);
    auto roster = OfficeRoster();
    roster.members.back().seat = Cluster::MemberSeat::Voter; // this node, promoted
    roster.members.back().raftEndpoint = "this-pc:6680";
    roster.members.push_back(Cluster::RosterMember { .id = "laptop",
                                                     .raftEndpoint = {},
                                                     .seat = Cluster::MemberSeat::Learner,
                                                     .publicKey = Testing::TestKeyPair("laptop").PublicKey(),
                                                     .schedulerEndpoint = {} });
    voterRecord.fleet = Cluster::FleetMembership { .clusterId = "fleet-c",
                                                   .roster = Cluster::EncodeRoster(roster),
                                                   .createdAtUnixSeconds = 0,
                                                   .admittedBy = Testing::TestKeyPair("office").PublicKey() };
    auto const voter = named(FormedBy(voterRecord));
    REQUIRE(RunsConsensus(voter));
    REQUIRE(Unwrap(voter.formation).fleetMembers.size() == 2); // office and laptop, never this node
    CHECK(StartupPolicyRejection(voter).value_or("(none)") == "(none)");
}

TEST_CASE("The surface map prints the mode and a learner's closed Raft port with why", "[node][formation][surfaces]")
{
    auto const cfg = FormedBy(RecordIn(Cluster::NodeMode::Learner));
    auto const text = RenderSurfaces(cfg);
    CHECK(text.starts_with("mode: learner\n"));
    CHECK(text.contains("not served (learner: dials the leader)"));
}

TEST_CASE("The surface map says when no record shaped the configuration and when none is minted yet",
          "[node][formation][surfaces]")
{
    auto const bare = RenderSurfaces(NodeConfig {});
    CHECK(bare.starts_with("mode: none"));
    CHECK(bare.contains("not served (no formation record yet)"));

    // The view `main` applies before the first start has minted anything: solitary, no cluster id.
    auto prospective = NodeConfig {};
    REQUIRE(ApplyFormation(prospective, Cluster::FormationRecord {}, {}).has_value());
    auto const text = RenderSurfaces(prospective);
    CHECK(text.starts_with("mode: solitary (no cluster minted yet"));
    CHECK_FALSE(text.contains("not served (no formation record yet)"));
    // An unminted record names no cluster, and the flag's value is left as it was.
    CHECK(prospective.clusterId == NodeConfig {}.clusterId);
}

TEST_CASE("A record committing this node to a cluster the pin does not name refuses the start, by name",
          "[node][formation][pin][security]")
{
    // What each mode commits the node to: nothing for a solitary node, the fleet it asked for a pending
    // one, the fleet it is in for a learner, its own cluster for a founding voter.
    auto pending = RecordIn(Cluster::NodeMode::Pending);
    pending.joining = Cluster::JoinTarget { .summary = CompileCacheWire::FleetSummary { .clusterId = "asked-c" },
                                            .provenKey = Testing::TestKeyPair("office").PublicKey(),
                                            .askedAtUnixSeconds = 42 };
    struct Row
    {
        std::string_view name;           ///< What the row pins.
        Cluster::FormationRecord record; ///< The record.
        std::string_view committed;      ///< The cluster it commits the node to; empty for none.
        std::string_view signer;         ///< Whose key vouches for it: the proven key, a voter's, its own.
    };
    auto const rows = std::array {
        Row { .name = "solitary", .record = RecordIn(Cluster::NodeMode::Solitary), .committed = {}, .signer = {} },
        Row { .name = "pending", .record = pending, .committed = "asked-c", .signer = "office" },
        Row {
            .name = "learner", .record = RecordIn(Cluster::NodeMode::Learner), .committed = "fleet-c", .signer = "office" },
        Row { .name = "founding voter",
              .record = RecordIn(Cluster::NodeMode::Voter),
              .committed = "own-c",
              .signer = ThisNode },
    };
    auto const pinned = [](std::string_view clusterId, std::string_view voter) {
        auto cfg = NodeConfig {};
        cfg.nodeId = ThisNode;
        cfg.identityPublicKey = Testing::TestKeyPair(ThisNode).PublicKey();
        cfg.fleetPin = Cluster::PinnedFleet { .clusterId = std::string { clusterId },
                                              .voterKeys = { Testing::TestKeyPair(std::string { voter }).PublicKey() } };
        return cfg;
    };
    for (auto const& row: rows)
    {
        INFO(row.name);
        CHECK(Cluster::CommittedClusterId(row.record).value_or("") == row.committed);

        // Unpinned, the control: every record shapes a configuration.
        auto unpinned = NodeConfig {};
        unpinned.nodeId = ThisNode;
        CHECK(ApplyFormation(unpinned, row.record, {}).has_value());

        // Pinned to the cluster it commits the node to, under the key that vouches for it: shaped.
        if (!row.committed.empty())
        {
            auto there = pinned(row.committed, row.signer);
            CHECK(ApplyFormation(there, row.record, {}).has_value());
        }

        // Pinned elsewhere, and pinned to the right cluster under a key nothing in the record vouches
        // with: both refused by name, with the remedy -- unless the record commits the node to nothing,
        // which a pin to another cluster names where it is GOING.
        for (auto [clusterId, voter]: { std::pair { std::string_view { "pinned-c" }, row.signer },
                                        std::pair { row.committed, std::string_view { "n-rogue" } } })
        {
            INFO("pinned to " << clusterId << " under " << voter << "'s key");
            auto refusedCfg = pinned(clusterId, voter);
            auto const applied = ApplyFormation(refusedCfg, row.record, {});
            if (row.committed.empty())
            {
                CHECK(applied.has_value());
                continue;
            }
            REQUIRE_FALSE(applied.has_value());
            CHECK(applied.error().contains(std::format("commits this node to cluster {}", row.committed)));
            CHECK(applied.error().contains(std::format("--fleet-id pins it to {}@", clusterId)));
            CHECK(applied.error().contains("reset its state directory"));
            CHECK_FALSE(refusedCfg.formation.has_value()); // nothing shaped from a record the pin refuses
        }
    }
}

TEST_CASE("A joined record is judged by the key that signed its admission, never by the voters its roster lists",
          "[node][formation][pin][security]")
{
    // An approval forged under n-rogue's key, its roster listing the pinned voter's: a roster is public
    // keys, so it lists whichever it likes. The start is a check of its own on the answer the record
    // came from, so it asks the key that SIGNED it.
    auto const pinnedToOffice = [] {
        auto cfg = NodeConfig {};
        cfg.nodeId = ThisNode;
        cfg.identityPublicKey = Testing::TestKeyPair(ThisNode).PublicKey();
        cfg.fleetPin =
            Cluster::PinnedFleet { .clusterId = "fleet-c", .voterKeys = { Testing::TestKeyPair("office").PublicKey() } };
        return cfg;
    };
    auto forged = RecordIn(Cluster::NodeMode::Learner);
    REQUIRE(forged.fleet.has_value());
    auto fleet = Unwrap(forged.fleet);
    auto const roster = Cluster::DecodeRoster(fleet.roster);
    REQUIRE(roster.has_value());
    REQUIRE(std::ranges::any_of(roster->members, [](Cluster::RosterMember const& member) {
        return member.seat == Cluster::MemberSeat::Voter
               && member.publicKey == std::optional { Testing::TestKeyPair("office").PublicKey() };
    })); // the pinned voter IS in the roster: what a check of the roster would wave through
    fleet.admittedBy = Testing::TestKeyPair("n-rogue").PublicKey();
    forged.fleet = fleet;

    auto refusedCfg = pinnedToOffice();
    auto const refused = ApplyFormation(refusedCfg, forged, {});
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().contains("commits this node to cluster fleet-c"));
    CHECK(refused.error().contains(
        std::format("under key {}", FormatEd25519PublicKey(Testing::TestKeyPair("n-rogue").PublicKey()))));
    CHECK_FALSE(refusedCfg.formation.has_value());

    // The control: the same record admitted under the pinned voter's key starts.
    auto genuine = forged;
    fleet.admittedBy = Testing::TestKeyPair("office").PublicKey();
    genuine.fleet = fleet;
    auto genuineCfg = pinnedToOffice();
    CHECK(ApplyFormation(genuineCfg, genuine, {}).has_value());
}

TEST_CASE("The surface map names the cluster this node is in and the pin, or says there is none",
          "[node][formation][surfaces][pin]")
{
    auto unpinned = FormedBy(RecordIn(Cluster::NodeMode::Learner));
    auto const text = RenderSurfaces(unpinned);
    CHECK(text.contains("\nfleet:\n  cluster    fleet-c\n"));
    CHECK(text.contains("  pinned to  none (--fleet-id unset: discovery is trust-on-first-use)\n"));

    auto pinned = NodeConfig {};
    pinned.nodeId = ThisNode;
    pinned.fleetPin =
        Cluster::PinnedFleet { .clusterId = "fleet-c", .voterKeys = { Testing::TestKeyPair("office").PublicKey() } };
    REQUIRE(ApplyFormation(pinned, RecordIn(Cluster::NodeMode::Learner), {}).has_value());
    CHECK(RenderSurfaces(pinned).contains(
        std::format("  pinned to  {}\n", Cluster::FormatPinnedFleet(Unwrap(pinned.fleetPin)))));

    // Before the first start has minted anything, the cluster is said to be none yet, never blank.
    auto prospective = NodeConfig {};
    REQUIRE(ApplyFormation(prospective, Cluster::FormationRecord {}, {}).has_value());
    CHECK(RenderSurfaces(prospective).contains("  cluster    none minted yet\n"));
}

TEST_CASE("A node pinned to another fleet records nobody, and one pinned to its own does",
          "[node][formation][pin][enrollment]")
{
    // A solitary node serves enrollment for the cluster it minted -- unless its pin names another,
    // which it is on its way to: anybody it admitted would found a fleet its own pin then refuses.
    auto cfg = FormedBy(RecordIn(Cluster::NodeMode::Solitary));
    cfg.identityPublicKey = Testing::TestKeyPair(ThisNode).PublicKey();
    REQUIRE(RunsConsensus(cfg));
    CHECK(ServesEnrollment(cfg, true)); // unpinned, the control

    auto const pinTo = [](std::string_view clusterId, std::string_view voter) {
        return Cluster::PinnedFleet { .clusterId = std::string { clusterId },
                                      .voterKeys = { Testing::TestKeyPair(std::string { voter }).PublicKey() } };
    };
    cfg.fleetPin = pinTo("own-c", ThisNode);
    CHECK(ServesEnrollment(cfg, true));

    cfg.fleetPin = pinTo("pinned-c", ThisNode);
    CHECK_FALSE(ServesEnrollment(cfg, true));

    // Its own cluster, but not under its own key: every answer it signed would be refused by the
    // joiners that pin, so it records nobody.
    cfg.fleetPin = pinTo("own-c", "n-office");
    CHECK_FALSE(ServesEnrollment(cfg, true));
}

TEST_CASE("A node that records nobody knows which reason, and serves exactly when it names none",
          "[node][formation][pin][enrollment]")
{
    // The reason is what a joiner pointed here is told (`EnrollmentAbsenceTable`), so each one is
    // derived from the configuration that produces it -- and `ServesEnrollment` agrees with every row.
    auto const solitary = [] {
        auto cfg = FormedBy(RecordIn(Cluster::NodeMode::Solitary));
        cfg.identityPublicKey = Testing::TestKeyPair(ThisNode).PublicKey();
        return cfg;
    };
    auto const pinTo = [](std::string_view clusterId, std::string_view voter) {
        return Cluster::PinnedFleet { .clusterId = std::string { clusterId },
                                      .voterKeys = { Testing::TestKeyPair(std::string { voter }).PublicKey() } };
    };
    struct Row
    {
        std::string_view name;                     ///< What the row configures.
        NodeConfig cfg;                            ///< The configuration.
        bool schedulerRuns;                        ///< Whether a scheduler tier was built.
        std::optional<EnrollmentAbsence> expected; ///< The reason; nothing when it serves.
    };
    auto unformed = NodeConfig {};
    unformed.nodeId = ThisNode;
    auto closed = solitary();
    closed.raftListen.clear(); // `--listen-raft=`: consensus closed by the operator
    closed.raftListenExplicit = true;
    auto confined = solitary();
    ApplyHostNames(confined, Withheld());
    auto const learner = FormedBy(RecordIn(Cluster::NodeMode::Learner));
    REQUIRE(RunsConsensus(learner));
    auto pinnedElsewhere = solitary();
    pinnedElsewhere.fleetPin = pinTo("pinned-c", ThisNode);
    auto notItsKey = solitary();
    notItsKey.fleetPin = pinTo("own-c", "n-office");
    auto pinnedHere = solitary();
    pinnedHere.fleetPin = pinTo("own-c", ThisNode);

    auto const rows = std::array {
        Row { .name = "no formation record",
              .cfg = unformed,
              .schedulerRuns = false,
              .expected = EnrollmentAbsence::NoConsensus },
        Row {
            .name = "consensus closed", .cfg = closed, .schedulerRuns = false, .expected = EnrollmentAbsence::NoConsensus },
        Row { .name = "a name that reaches only this machine",
              .cfg = confined,
              .schedulerRuns = false,
              .expected = EnrollmentAbsence::ConfinedToThisMachine },
        Row { .name = "a learner", .cfg = learner, .schedulerRuns = false, .expected = EnrollmentAbsence::NoScheduler },
        Row { .name = "pinned to another cluster",
              .cfg = pinnedElsewhere,
              .schedulerRuns = true,
              .expected = EnrollmentAbsence::PinnedToAnotherCluster },
        Row { .name = "pinned to its own cluster under another key",
              .cfg = notItsKey,
              .schedulerRuns = true,
              .expected = EnrollmentAbsence::NotAPinnedVoter },
        Row { .name = "pinned to its own cluster under its own key",
              .cfg = pinnedHere,
              .schedulerRuns = true,
              .expected = std::nullopt },
        Row { .name = "unpinned", .cfg = solitary(), .schedulerRuns = true, .expected = std::nullopt },
    };
    for (auto const& row: rows)
    {
        INFO(row.name);
        CHECK(EnrollmentAbsenceOf(row.cfg, row.schedulerRuns) == row.expected);
        CHECK(ServesEnrollment(row.cfg, row.schedulerRuns) == !row.expected.has_value());
    }
    // The confined row runs its consensus alone on loopback rather than being closed: the reason is
    // the confinement's, never `NoConsensus`.
    CHECK(ConsensusConfinedToThisMachine(confined));
    CHECK(RunsConsensus(confined));
}

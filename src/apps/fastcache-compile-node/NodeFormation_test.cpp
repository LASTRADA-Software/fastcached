// SPDX-License-Identifier: Apache-2.0
#include "ConsensusTier.hpp"
#include "NodeConfig.hpp"
#include "NodeDefaults.hpp"
#include "NodeFormation.hpp"
#include "NodeSurfaces.hpp"

#include <FastCache/Cluster/FleetEndpoints.hpp>
#include <FastCache/Cluster/FormationRecord.hpp>
#include <FastCache/Cluster/NodeMode.hpp>
#include <FastCache/Cluster/Roster.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <optional>
#include <string>
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
                                                                  .publicKey = Testing::TestKeyPair("office").PublicKey() },
                                          Cluster::RosterMember { .id = ThisNode,
                                                                  .raftEndpoint = {},
                                                                  .seat = Cluster::MemberSeat::Learner,
                                                                  .publicKey =
                                                                      Testing::TestKeyPair(ThisNode).PublicKey() } },
                             .principals = {},
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
                                                  .createdAtUnixSeconds = 0 };
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
    };
    for (auto const row: { Row { .mode = Solitary, .consensus = true, .raftPort = true, .scheduler = true },
                           Row { .mode = Pending, .consensus = true, .raftPort = true, .scheduler = true },
                           Row { .mode = Learner, .consensus = true, .raftPort = false, .scheduler = false },
                           Row { .mode = Voter, .consensus = true, .raftPort = true, .scheduler = true } })
    {
        INFO(Cluster::NodeModeRowFor(row.mode).name);
        auto const cfg = FormedBy(RecordIn(row.mode));
        CHECK(RunsConsensus(cfg) == row.consensus);
        CHECK(RowFor(NodeSurface::Raft).Resolve(cfg).empty() == !row.raftPort);
        CHECK(ServesScheduler(cfg) == row.scheduler);
    }
}

TEST_CASE("A configuration no record shaped runs no consensus and opens no raft port", "[node][formation][mode]")
{
    auto const cfg = NodeConfig {};
    CHECK_FALSE(RunsConsensus(cfg));
    CHECK_FALSE(ServesScheduler(cfg));
    CHECK(RowFor(NodeSurface::Raft).Resolve(cfg).empty());
    CHECK(SchedulersOf(cfg).empty());
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
    CHECK(SchedulersOf(pending) == SchedulersOf(solitary));
    CHECK(SchedulersOf(pending) == std::vector<std::string> { "127.0.0.1:6674" });
    CHECK(BootstrapMembersOf(pending).size() == 1);
    CHECK(Unwrap(pending.formation).clusterId == Unwrap(solitary.formation).clusterId);
    CHECK(pending.clusterId == "own-c");
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
    CHECK(SchedulersOf(cfg) == std::vector<std::string> { "office:6674" });
    auto const members = BootstrapMembersOf(cfg);
    // Never itself: its own seat is the leader's to replicate.
    CHECK(std::ranges::none_of(members, [&](auto const& m) { return m.id == cfg.nodeId; }));
    CHECK(std::ranges::contains(members, std::string { "office" }, &Cluster::ClusterMember::id));
}

TEST_CASE("Remembered endpoints of another fleet are not registered with", "[node][formation][mode]")
{
    // A fleet-endpoints file outlives the fleet it was written for; a node that joined another one
    // must not hand its worker to the old fleet's voters.
    auto const stale =
        Cluster::FleetEndpoints { .clusterId = "old-fleet",
                                  .voters = { { .id = "gone", .raftEndpoint = "gone:6680", .nodeEndpoint = "gone:6674" } } };
    auto const cfg = FormedBy(RecordIn(Cluster::NodeMode::Learner), stale);
    CHECK(SchedulersOf(cfg).empty());
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
                                                      .createdAtUnixSeconds = 0 };
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
                                              .createdAtUnixSeconds = 0 };
    auto cfg = NodeConfig {};
    auto const applied = ApplyFormation(cfg, record, {});
    REQUIRE_FALSE(applied.has_value());
    CHECK(applied.error().contains("fleet-c"));
    CHECK(applied.error().contains("roster"));
    // Nothing half-applied: a configuration the start refuses is not one anything reads.
    CHECK_FALSE(cfg.formation.has_value());
}

TEST_CASE("A machine whose name reaches only itself stands down or is refused under every mode",
          "[node][formation][mode][defaults]")
{
    // Walked over the mode TABLE, so a mode added tomorrow is judged here with no edit. The
    // expectation is read off the same two columns the rule reads: a mode that opens the port
    // stands consensus down on a name nobody else can dial, a learner dials out and needs no
    // name, and a mode other machines dial is refused by name rather than standing down.
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

        CHECK(RunsConsensus(cfg) == !ModeOpensRaftPort(mode.mode));
        CHECK(RowFor(NodeSurface::Raft).Resolve(cfg).empty());
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
            for (auto const& members: { BootstrapMembersOf(cfg), std::vector<Cluster::ClusterMember> {} })
            {
                INFO("members: " << members.size());
                auto const self = ConsensusSelfMemberOf(cfg, members);
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
    worker.schedulers = SchedulersOf(worker);
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
    SECTION("a scheduler on another machine")
    {
        worker.schedulers = { "sched.corp.example:6675" };
        CHECK(AdvertisedEndpoint(worker).empty());
        CHECK(StartupPolicyRejection(worker) == std::optional { std::string { WorkerNameReachesOnlyThisMachineRefusal } });
    }
    SECTION("any one of several schedulers on another machine")
    {
        worker.schedulers = { "127.0.0.1:6674", "sched.corp.example:6675" };
        CHECK(StartupPolicyRejection(worker) == std::optional { std::string { WorkerNameReachesOnlyThisMachineRefusal } });
    }
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

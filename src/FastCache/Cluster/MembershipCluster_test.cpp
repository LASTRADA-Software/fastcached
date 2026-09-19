// SPDX-License-Identifier: Apache-2.0
//
// The membership policy driven over a real Raft cluster: every command replicated through
// the log, every configuration change committed by the quorum it names, every message
// authenticated. `MembershipPolicy_test` pins each rule as a pure function; what only a
// cluster can show is what the CONSEQUENCES of a rule do to consensus -- who steps down,
// who is elected, what commits.
#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Cluster/MembershipPolicy.hpp>
#include <FastCache/Cluster/RosterKeys.hpp>
#include <FastCache/Consensus/IRaftPeerIdentity.hpp>
#include <FastCache/Consensus/RaftClusterHarness.hpp>
#include <FastCache/Consensus/RaftMembership.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Cluster;
using FastCache::Testing::Unwrap;

namespace
{
/// Where member `n<k>` answers consensus: a machine of its own, `10.0.0.<k>`.
/// @param id The member.
/// @return Its consensus endpoint.
[[nodiscard]] std::string EndpointOf(Consensus::NodeId const& id)
{
    return std::format("10.0.0.{}:6680", id.substr(1));
}

/// Every machine a case here can start, a later joiner included.
constexpr std::array<char const*, 5> EveryMachine { "n1", "n2", "n3", "n4", "n5" };

/// One PRODUCTION roster per machine, each over every machine typed with its key -- what
/// `--raft-peer id=host:port@<key>` on every command line gives each node's `RosterKeys`.
///
/// Production rather than `Testing::SharedRoster`, and that is the point (#1555): a roster
/// that never adopts the state was MORE permissive than any node, so a forget that revoked
/// a leader's key while its removal was still to commit was a case this harness could not
/// reach. Each roster adopts its OWN node's applied state and configuration
/// (`Fleet::AdoptRosters`), as `ConsensusTier` has its roster do.
/// @return The rosters, by machine.
[[nodiscard]] std::map<Consensus::NodeId, std::unique_ptr<RosterKeys>> Rosters()
{
    auto typed = std::vector<ClusterMember> {};
    for (auto const* const id: EveryMachine)
        typed.push_back(ClusterMember { .id = id,
                                        .raftEndpoint = EndpointOf(id),
                                        .schedulerEndpoint = {},
                                        .schedulerEndpointHistory = SchedulerEndpointHistory::NeverAnnounced,
                                        .seat = MemberSeat::Voter,
                                        .publicKey = Testing::TestKeyPair(id).PublicKey() });

    auto rosters = std::map<Consensus::NodeId, std::unique_ptr<RosterKeys>> {};
    for (auto const* const id: EveryMachine)
        rosters.emplace(id, std::make_unique<RosterKeys>(Testing::TestKeyPair(id), typed));
    return rosters;
}

/// Who every member is: itself, proved with its own key, judging every peer by its own roster.
/// @param rosters Every machine's roster; must outlive the harness.
/// @return The factory the harness requires.
[[nodiscard]] Consensus::RaftClusterHarness::IdentityFactory Identities(
    std::map<Consensus::NodeId, std::unique_ptr<RosterKeys>> const& rosters)
{
    return [&rosters](Consensus::NodeId const& who) -> std::unique_ptr<Consensus::IRaftPeerIdentity const> {
        return std::make_unique<Consensus::RaftPeerIdentity const>(who, *rosters.at(who));
    };
}

/// The host a forget of `id` tombstones.
/// @param id The member.
/// @return Its host.
[[nodiscard]] std::string HostOf(Consensus::NodeId const& id)
{
    return std::format("10.0.0.{}", id.substr(1));
}

/// Whether `state` records `id`.
/// @param state A node's applied state.
/// @param id The member.
/// @return True when a record names it.
[[nodiscard]] bool Records(ClusterState const& state, Consensus::NodeId const& id)
{
    return std::ranges::find(state.members, id, &ClusterMember::id) != state.members.end();
}

/// A fleet: a Raft cluster whose log carries `Cluster::Command`s, reconciled by whoever
/// leads exactly as `ConsensusTier::Reconcile` does it.
///
/// Every member was started with every other on its command line and desires every
/// other, as discovery hands proven peers over -- the shape in which a forget has the
/// most reasons not to stick.
class Fleet
{
  public:
    /// @param ids Every member, each a voter.
    explicit Fleet(std::vector<Consensus::NodeId> ids):
        _ids { ids },
        _rosters { Rosters() },
        _cluster { std::move(ids), Identities(_rosters) }
    {
    }

    /// @param configuration The voters and the learners, every one bootstrapped with both.
    explicit Fleet(Consensus::Configuration const& configuration):
        _ids { MembersOf(configuration) },
        _rosters { Rosters() },
        _cluster { configuration, Identities(_rosters) }
    {
    }

    Fleet(Fleet const&) = delete;
    Fleet(Fleet&&) = delete;
    Fleet& operator=(Fleet const&) = delete;
    Fleet& operator=(Fleet&&) = delete;
    ~Fleet() = default;

    /// Start @p id, a machine no member names yet, as `--raft-join` starts one.
    /// @param id The machine.
    void Join(Consensus::NodeId const& id)
    {
        _cluster.Join(id);
        _ids.push_back(id);
    }

    /// @return The cluster underneath.
    [[nodiscard]] Consensus::RaftClusterHarness& Cluster() noexcept
    {
        return _cluster;
    }

    /// The cluster state `who` has applied: its committed commands through `Apply`, as
    /// `ClusterStateMachine` builds it.
    /// @param who The member.
    /// @return Its state.
    [[nodiscard]] ClusterState StateAt(Consensus::NodeId const& who) const
    {
        ClusterState state;
        for (auto const& entry: _cluster.At(who).applied)
            if (auto const command = DecodeCommand(entry.payload); command.has_value())
                Apply(state, *command);
        return state;
    }

    /// One reconcile pass on whoever leads: the leader half of `ConsensusTier::Reconcile`,
    /// through the two functions it calls.
    /// @return What the pass refused because its host was forgotten.
    std::vector<DesiredMember> Pass()
    {
        AdoptRosters();

        auto const leader = _cluster.Leader();
        if (!leader.has_value())
            return {};

        // One read of the driver, as the tier makes it: the configuration, the commit
        // index and every member's match index describe one moment.
        auto const state = StateAt(*leader);
        auto const progress = _cluster.At(*leader).driver->CurrentProgress();
        auto const& configuration = progress.configuration;
        auto const plan = MembershipProposals(state, configuration, DesiredBy(*leader));
        for (auto const& command: plan.proposals)
            std::ignore = _cluster.ProposeOnLeader(Encode(command));

        auto const self = ClusterMember { .id = *leader,
                                          .raftEndpoint = EndpointOf(*leader),
                                          .schedulerEndpoint = {},
                                          .schedulerEndpointHistory = SchedulerEndpointHistory::NeverAnnounced,
                                          .seat = MemberSeat::Voter,
                                          .publicKey = std::nullopt };
        auto const quorum =
            NextQuorumChange(state,
                             configuration,
                             self,
                             _ids,
                             Replication { .commitIndex = progress.commitIndex, .matchIndex = progress.matchIndex });
        _catchingUp = quorum.catchingUp;
        if (quorum.change.has_value())
            std::ignore = _cluster.ProposeMembershipOnLeader(*quorum.change);
        return plan.forgotten;
    }

    /// `--cluster-forget=<id>`, as `ConsensusTier::Propose` takes it.
    /// @param id The member.
    /// @return Nothing when proposed; the refusal otherwise.
    std::expected<void, ConsensusError> Forget(Consensus::NodeId const& id)
    {
        auto const leader = _cluster.Leader();
        if (!leader.has_value())
            return std::unexpected { ConsensusError {
                .code = ConsensusErrorCode::NotLeader, .context = "nobody leads", .knownLeader = std::nullopt } };

        auto prepared = PrepareForget(
            _cluster.At(*leader).driver->CurrentProgress().configuration, id, _rosters.at(*leader)->KeysOf(id).live);
        if (!prepared.has_value())
            return std::unexpected { prepared.error() };

        std::ignore = _cluster.ProposeOnLeader(Encode(*prepared));
        return {};
    }

    /// `--cluster-admit=<id>=<endpoint>`: an operator's voter record, committed directly.
    /// @param id The member.
    /// @return Whether a leader took it.
    bool Admit(Consensus::NodeId const& id)
    {
        return _cluster
            .ProposeOnLeader(Encode(Command { .kind = CommandKind::AddMember,
                                              .key = id,
                                              .value = EndpointOf(id),
                                              .schedulerEndpoint = {},
                                              .publicKey = std::nullopt,
                                              .role = std::nullopt }))
            .has_value();
    }

    /// Every member's roster adopts its own node's applied state and configuration -- what
    /// `ConsensusTier::OnStateChanged` and `ConsensusTier::ReportQuorum` have the tier's roster
    /// adopt (#1555). Once per pass, where the tier adopts the state at every apply: the lag
    /// keeps a revoked key live a little LONGER here, never shorter.
    void AdoptRosters()
    {
        for (auto const& id: _ids)
        {
            auto& roster = *_rosters.at(id);
            roster.Adopt(StateAt(id));
            roster.AdoptConfiguration(_cluster.At(id).driver->Node().ActiveConfiguration());
        }
    }

    /// Who the last pass said a promotion is waiting for (#1537).
    /// @return Their ids.
    [[nodiscard]] std::vector<Consensus::NodeId> const& CatchingUp() const noexcept
    {
        return _catchingUp;
    }

    /// Run reconcile passes, `StepsPerPass` apart.
    /// @param passes How many.
    void Reconcile(std::size_t passes);

  private:
    /// Every member a configuration names, voters first.
    /// @param configuration The configuration.
    /// @return Its ids.
    [[nodiscard]] static std::vector<Consensus::NodeId> MembersOf(Consensus::Configuration const& configuration)
    {
        auto ids = configuration.voters;
        ids.insert(ids.end(), configuration.learners.begin(), configuration.learners.end());
        return ids;
    }

    /// What `who` desires: itself, asserting its (empty) scheduler endpoint, and every
    /// other member with no opinion about one, as discovery hands proven peers over.
    /// @param who The member.
    /// @return Its desires.
    [[nodiscard]] std::vector<DesiredMember> DesiredBy(Consensus::NodeId const& who) const
    {
        auto desired = std::vector<DesiredMember> {};
        for (auto const& id: _ids)
            desired.push_back(
                DesiredMember { .id = id,
                                .raftEndpoint = EndpointOf(id),
                                .schedulerEndpoint = id == who ? std::optional { std::string {} } : std::nullopt,
                                .publicKey = std::nullopt });
        return desired;
    }

    std::vector<Consensus::NodeId> _ids;

    /// Every machine's own roster. Before `_cluster`, whose identities read them.
    std::map<Consensus::NodeId, std::unique_ptr<RosterKeys>> _rosters;

    Consensus::RaftClusterHarness _cluster;
    std::vector<Consensus::NodeId> _catchingUp;
};

/// Step until one leader exists, or give up.
/// @param cluster The cluster.
/// @return Whether one emerged.
[[nodiscard]] bool SettleOnLeader(Consensus::RaftClusterHarness& cluster)
{
    for ([[maybe_unused]] auto const step: std::views::iota(0, 300))
    {
        cluster.Step();
        if (cluster.Leader().has_value())
            return true;
    }
    return false;
}

/// Every safety property, by name.
/// @param cluster The cluster.
void RequireNoViolations(Consensus::RaftClusterHarness const& cluster)
{
    for (auto const& violation: cluster.Violations())
        FAIL_CHECK(violation);
    REQUIRE(cluster.Violations().empty());
}

/// Harness steps between two reconcile passes: 100 ms, against the tier's second.
constexpr std::size_t StepsPerPass = 10;

void Fleet::Reconcile(std::size_t passes)
{
    for ([[maybe_unused]] auto const pass: std::views::iota(std::size_t { 0 }, passes))
    {
        _cluster.Run(StepsPerPass);
        std::ignore = Pass();
    }
}

/// A setting write: the ordinary entry a cluster that commits nothing cannot take.
/// @param value The value `lease-lifetime` is set to.
/// @return The command's bytes.
[[nodiscard]] std::vector<std::byte> SettingWrite(std::string value)
{
    return Encode(Command { .kind = CommandKind::SetSetting,
                            .key = "lease-lifetime",
                            .value = std::move(value),
                            .schedulerEndpoint = {},
                            .publicKey = std::nullopt,
                            .role = std::nullopt });
}
} // namespace

TEST_CASE("A cluster that forgets its leader commits a configuration without it, and elects another",
          "[consensus][cluster][membership][forget]")
{
    // #1539. The leader is the one member whose forget no pass acted on: a leader never
    // proposed its own removal, and every leader's bootstrap set contains itself.
    //
    // And #1555: the forget revokes the leader's key, and every member's roster -- production
    // `RosterKeys`, adopting each node's own state and configuration -- keeps that key live for
    // the leader alone while its configuration still counts it. Cut off at the commit instead,
    // it could not commit the removal it proposes for itself.
    Fleet fleet { { "n1", "n2", "n3" } };
    REQUIRE(SettleOnLeader(fleet.Cluster()));
    for ([[maybe_unused]] auto const pass: std::views::iota(0, 10))
    {
        fleet.Cluster().Run(StepsPerPass);
        std::ignore = fleet.Pass();
    }

    auto const forgotten = Unwrap(fleet.Cluster().Leader());
    REQUIRE(Records(fleet.StateAt(forgotten), forgotten));
    auto others = std::vector<Consensus::NodeId> { "n1", "n2", "n3" };
    std::erase(others, forgotten);

    REQUIRE(fleet.Forget(forgotten).has_value());

    auto removed = false;
    auto refusedBySuccessor = false;
    for (auto const pass: std::views::iota(0, 60))
    {
        fleet.Cluster().Run(StepsPerPass);

        // Counted only on a pass another member leads: the forgotten leader refuses its
        // OWN desire too, and that is not the refusal this case is about.
        auto const leading = fleet.Cluster().Leader();
        for (auto const& refused: fleet.Pass())
            refusedBySuccessor = refusedBySuccessor || (leading != forgotten && refused.id == forgotten);

        INFO("reconcile pass " << pass);
        for (auto const& id: others)
        {
            auto const counted =
                Consensus::Membership::IsMember(fleet.Cluster().At(id).driver->Node().ActiveConfiguration(), forgotten);
            // Once out, never back: neither re-recorded nor re-counted.
            if (removed)
                CHECK_FALSE(counted);
            removed = removed || !counted;
        }
    }

    // Both of the others hold the configuration without it...
    for (auto const& id: others)
    {
        CAPTURE(id);
        auto const& node = fleet.Cluster().At(id).driver->Node();
        CHECK_FALSE(Consensus::Membership::IsMember(node.ActiveConfiguration(), forgotten));
        CHECK(node.ActiveConfiguration().voters.size() == 2);
    }

    // ...and it COMMITTED: a removed leader steps down only once its removal commits
    // (`RaftNode`, §4.2.2), and one of the other two now leads.
    CHECK(fleet.Cluster().At(forgotten).driver->Node().CurrentRole() != Consensus::Role::Leader);
    auto const successor = Unwrap(fleet.Cluster().Leader());
    CHECK(successor != forgotten);

    // It never re-entered the record, and the new leader said why it would not.
    auto const state = fleet.StateAt(successor);
    CHECK_FALSE(Records(state, forgotten));
    CHECK(state.HasForgotten(HostOf(forgotten)));
    CHECK(std::ranges::contains(state.revokedKeys, forgotten, &RevokedKey::id));
    CHECK(refusedBySuccessor);

    // And the cluster that is left still commits.
    REQUIRE(fleet.Cluster()
                .ProposeOnLeader(Encode(Command { .kind = CommandKind::SetSetting,
                                                  .key = "lease-lifetime",
                                                  .value = "20min",
                                                  .schedulerEndpoint = {},
                                                  .publicKey = std::nullopt,
                                                  .role = std::nullopt }))
                .has_value());
    fleet.Cluster().Run(60);
    CHECK(fleet.StateAt(successor).SettingOf("lease-lifetime") == "20min");
    RequireNoViolations(fleet.Cluster());
}

TEST_CASE("A cluster that forgets a follower keeps its leader, and takes the follower out of its quorum",
          "[consensus][cluster][membership][forget]")
{
    // The control for the case above: the forgotten member is not the leader, so the leader
    // stays. Every member here was typed into every other's `--raft-peer`, so the leader's
    // bootstrap set names the follower -- an operator's assertion, which kept a forgotten
    // follower counted until its forget revoked its key (#1555). Counted, it keeps that key
    // for itself, so a forgotten follower nobody removed would go on voting: it leaves the
    // quorum, and only then is it refused.
    Fleet fleet { { "n1", "n2", "n3" } };
    REQUIRE(SettleOnLeader(fleet.Cluster()));
    for ([[maybe_unused]] auto const pass: std::views::iota(0, 10))
    {
        fleet.Cluster().Run(StepsPerPass);
        std::ignore = fleet.Pass();
    }

    auto const leader = Unwrap(fleet.Cluster().Leader());
    auto const follower = leader == "n1" ? Consensus::NodeId { "n2" } : Consensus::NodeId { "n1" };
    REQUIRE(fleet.Forget(follower).has_value());

    for (auto const pass: std::views::iota(0, 40))
    {
        fleet.Cluster().Run(StepsPerPass);
        std::ignore = fleet.Pass();
        INFO("reconcile pass " << pass);
        CHECK(fleet.Cluster().Leader() == leader);
    }

    auto const state = fleet.StateAt(leader);
    CHECK_FALSE(Records(state, follower));
    CHECK(state.HasForgotten(HostOf(follower)));
    CHECK(std::ranges::contains(state.revokedKeys, follower, &RevokedKey::id));
    auto const& active = fleet.Cluster().At(leader).driver->Node().ActiveConfiguration();
    CHECK_FALSE(Consensus::Membership::IsMember(active, follower));
    CHECK(active.voters.size() == 2);
    RequireNoViolations(fleet.Cluster());
}

TEST_CASE("Forgetting a running voter and losing the leader straight after does not wedge the cluster",
          "[consensus][cluster][membership][forget]")
{
    // #1555. Four voters, each typed into every other's `--raft-peer`. An operator forgets a
    // follower that is still running, and the leader is lost before any reconcile pass has
    // taken the follower out of the configuration. Cut off at the revocation, the forgotten
    // follower would leave two of four: no majority, so nobody could ever propose the removal
    // that would make two a majority -- a cluster wedged for good by one command and one
    // crash. Its key stays live FOR IT while the configuration counts it, so three of four
    // elect, and the new leader takes it out.
    Fleet fleet { { "n1", "n2", "n3", "n4" } };
    REQUIRE(SettleOnLeader(fleet.Cluster()));
    fleet.Reconcile(10);

    auto const leader = Unwrap(fleet.Cluster().Leader());
    auto survivors = std::vector<Consensus::NodeId> { "n1", "n2", "n3", "n4" };
    std::erase(survivors, leader);
    auto const forgotten = survivors.back();
    REQUIRE(fleet.Forget(forgotten).has_value());

    // The forget commits and every survivor applies it -- and ADOPTS it, which is the
    // revocation taking effect -- with no pass run, so the configuration still counts it.
    auto const appliedEverywhere = [&fleet, &survivors, &forgotten] {
        return std::ranges::all_of(survivors, [&fleet, &forgotten](Consensus::NodeId const& id) {
            return std::ranges::contains(fleet.StateAt(id).revokedKeys, forgotten, &RevokedKey::id);
        });
    };
    for ([[maybe_unused]] auto const step: std::views::iota(0, 100))
    {
        if (appliedEverywhere())
            break;
        fleet.Cluster().Step();
    }
    REQUIRE(appliedEverywhere());
    fleet.AdoptRosters();
    for (auto const& id: survivors)
    {
        CAPTURE(id);
        REQUIRE(Consensus::Membership::IsMember(fleet.Cluster().At(id).driver->Node().ActiveConfiguration(), forgotten));
    }

    // The leader is lost for good.
    fleet.Cluster().Partition({ leader });
    fleet.Reconcile(60);

    // Three of four elected one of themselves, which took the forgotten member out of the
    // configuration and goes on committing.
    auto const successor = fleet.Cluster().Leader();
    REQUIRE(successor.has_value());
    CHECK(Unwrap(successor) != leader);
    CHECK(Unwrap(successor) != forgotten);
    CHECK_FALSE(Consensus::Membership::IsMember(fleet.Cluster().At(Unwrap(successor)).driver->Node().ActiveConfiguration(),
                                                forgotten));
    REQUIRE(fleet.Cluster().ProposeOnLeader(SettingWrite("20min")).has_value());
    fleet.Reconcile(10);
    CHECK(fleet.StateAt(Unwrap(successor)).SettingOf("lease-lifetime") == "20min");
    RequireNoViolations(fleet.Cluster());
}

TEST_CASE("Forgetting the only voter is refused by name, and the cluster keeps it",
          "[consensus][cluster][membership][forget]")
{
    // A configuration with no voter commits nothing, so this forget could only ever leave
    // the record saying *forgotten* while the quorum went on counting the member.
    Fleet fleet { { "n1" } };
    REQUIRE(SettleOnLeader(fleet.Cluster()));
    for ([[maybe_unused]] auto const pass: std::views::iota(0, 5))
    {
        fleet.Cluster().Run(StepsPerPass);
        std::ignore = fleet.Pass();
    }

    auto const refused = fleet.Forget("n1");
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == ConsensusErrorCode::InvalidConfiguration);
    CHECK(refused.error().context.starts_with("cannot forget n1: it is the cluster's only voter"));

    fleet.Cluster().Run(50);
    CHECK(Records(fleet.StateAt("n1"), "n1"));
    CHECK_FALSE(fleet.StateAt("n1").HasForgotten(HostOf("n1")));
    CHECK(fleet.Cluster().Leader() == Consensus::NodeId { "n1" });
    RequireNoViolations(fleet.Cluster());
}

// --------------------------------------------------------------------------
// A voter is counted only once it has caught up (#1537).

TEST_CASE("Promoting a learner that is away does not stall the cluster's commits",
          "[consensus][cluster][membership][learner]")
{
    // #1537. `n1` is the only voter and `n2` a learner that has gone away. An operator
    // promotes it. Counted at once, `n2` would make every commit need both machines --
    // the promotion entry first of all -- so nothing commits until it returns.
    Fleet fleet { Consensus::Configuration { .voters = { "n1" }, .learners = { "n2" } } };
    REQUIRE(SettleOnLeader(fleet.Cluster()));
    fleet.Reconcile(10);
    REQUIRE(RecordedSeatOf(fleet.StateAt("n1"), "n2") == MemberSeat::Learner);

    fleet.Cluster().Partition({ "n2" });
    REQUIRE(fleet.Admit("n2"));
    fleet.Reconcile(10);
    REQUIRE(RecordedSeatOf(fleet.StateAt("n1"), "n2") == MemberSeat::Voter);

    REQUIRE(fleet.Cluster().ProposeOnLeader(SettingWrite("20min")).has_value());
    fleet.Reconcile(30);

    // Committed with `n2` away, by the leader that led before: its promotion is
    // waiting, not counted -- and the wait is named.
    CHECK(fleet.Cluster().Leader() == Consensus::NodeId { "n1" });
    CHECK(fleet.StateAt("n1").SettingOf("lease-lifetime") == "20min");
    CHECK(fleet.Cluster().At("n1").driver->Node().ActiveConfiguration()
          == Consensus::Configuration { .voters = { "n1" }, .learners = { "n2" } });
    CHECK(fleet.CatchingUp() == std::vector<Consensus::NodeId> { "n2" });

    // Back, it catches up and is promoted -- one change, and still committing.
    fleet.Cluster().Heal();
    fleet.Reconcile(30);
    CHECK(fleet.Cluster().At("n1").driver->Node().ActiveConfiguration()
          == Consensus::Configuration { .voters = { "n1", "n2" }, .learners = {} });
    CHECK(fleet.CatchingUp().empty());
    REQUIRE(fleet.Cluster().ProposeOnLeader(SettingWrite("30min")).has_value());
    fleet.Reconcile(10);
    CHECK(fleet.StateAt("n2").SettingOf("lease-lifetime") == "30min");
    RequireNoViolations(fleet.Cluster());
}

TEST_CASE("Admitting a voter that is not up does not stall the cluster's commits",
          "[consensus][cluster][membership][learner]")
{
    // The same fault reached through an admission: `--cluster-admit` of a machine that is
    // not running yet. Added straight to the voters, `n2` is counted before it has
    // answered once.
    Fleet fleet { std::vector<Consensus::NodeId> { "n1" } };
    REQUIRE(SettleOnLeader(fleet.Cluster()));
    fleet.Reconcile(5);

    fleet.Join("n2");
    fleet.Cluster().Partition({ "n2" });
    REQUIRE(fleet.Admit("n2"));
    fleet.Reconcile(10);

    REQUIRE(fleet.Cluster().ProposeOnLeader(SettingWrite("20min")).has_value());
    fleet.Reconcile(30);
    CHECK(fleet.Cluster().Leader() == Consensus::NodeId { "n1" });
    CHECK(fleet.StateAt("n1").SettingOf("lease-lifetime") == "20min");
    CHECK(fleet.Cluster().At("n1").driver->Node().ActiveConfiguration()
          == Consensus::Configuration { .voters = { "n1" }, .learners = { "n2" } });
    CHECK(fleet.CatchingUp() == std::vector<Consensus::NodeId> { "n2" });

    fleet.Cluster().Heal();
    fleet.Reconcile(40);
    CHECK(fleet.Cluster().At("n1").driver->Node().ActiveConfiguration()
          == Consensus::Configuration { .voters = { "n1", "n2" }, .learners = {} });
    RequireNoViolations(fleet.Cluster());
}

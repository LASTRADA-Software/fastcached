// SPDX-License-Identifier: Apache-2.0
//
// The membership policy driven over a real Raft cluster: every command replicated through
// the log, every configuration change committed by the quorum it names, every message
// authenticated. `MembershipPolicy_test` pins each rule as a pure function; what only a
// cluster can show is what the CONSEQUENCES of a rule do to consensus -- who steps down,
// who is elected, what commits.
#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Cluster/MembershipPolicy.hpp>
#include <FastCache/Consensus/IRaftPeerIdentity.hpp>
#include <FastCache/Consensus/RaftClusterHarness.hpp>
#include <FastCache/Consensus/RaftMembership.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <expected>
#include <format>
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
/// Who every member is: itself, under its own key, over one roster naming every machine a case
/// here starts -- a later joiner included, as an operator admitting it with its key makes it.
/// @return The factory the harness requires.
[[nodiscard]] Consensus::RaftClusterHarness::IdentityFactory Identities()
{
    auto roster =
        std::shared_ptr<Testing::SharedRoster const> { Testing::SharedRoster::Of({ "n1", "n2", "n3", "n4", "n5" }) };
    return
        [roster = std::move(roster)](Consensus::NodeId const& who) -> std::unique_ptr<Consensus::IRaftPeerIdentity const> {
            return Testing::TestPeerIdentity::Honest(who, roster);
        };
}

/// Where member `n<k>` answers consensus: a machine of its own, `10.0.0.<k>`.
/// @param id The member.
/// @return Its consensus endpoint.
[[nodiscard]] std::string EndpointOf(Consensus::NodeId const& id)
{
    return std::format("10.0.0.{}:6680", id.substr(1));
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
        _cluster { std::move(ids), Identities() }
    {
    }

    /// @param configuration The voters and the learners, every one bootstrapped with both.
    explicit Fleet(Consensus::Configuration const& configuration):
        _ids { MembersOf(configuration) },
        _cluster { configuration, Identities() }
    {
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

        if (auto allowed = ValidateForget(_cluster.At(*leader).driver->CurrentProgress().configuration, id);
            !allowed.has_value())
            return allowed;

        std::ignore = _cluster.ProposeOnLeader(Encode(Command { .kind = CommandKind::RemoveMember,
                                                                .key = id,
                                                                .value = {},
                                                                .schedulerEndpoint = {},
                                                                .publicKey = std::nullopt,
                                                                .role = std::nullopt }));
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

TEST_CASE("A cluster that forgets a follower keeps its leader and its quorum as before",
          "[consensus][cluster][membership][forget]")
{
    // The control: the rule is about THIS node's own entry, so a forgotten follower is
    // handled exactly as it always was. Every member here was typed into every other's
    // `--raft-peer`, so the leader's bootstrap set names the follower -- an operator's
    // assertion, and the quorum goes on counting it while its record goes.
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
    CHECK(fleet.Cluster().At(leader).driver->Node().ActiveConfiguration().voters.size() == 3);
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

    fleet.Cluster().Join("n2");
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

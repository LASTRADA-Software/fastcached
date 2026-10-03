// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Distributed/LeaseSigner.hpp>
#include <FastCache/Distributed/LeaseToken.hpp>
#include <FastCache/Distributed/StateLeaseRoster.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>

#include <core/platform/Clock.hpp>
#include <tests/RaftPeerKeyFakes.hpp>

using namespace FastCache;
using namespace FastCache::Distributed;
using namespace std::chrono_literals;
using FastCache::Testing::TestKeyPair;

namespace
{
/// A fixed instant, so a grant's expiry is a value a case can name.
constexpr auto Noon = std::chrono::system_clock::time_point { std::chrono::hours { 500'000 } };
} // namespace

TEST_CASE("A learner's roster answers a grant from its fleet's voter after days offline", "[distributed][roster][formation]")
{
    // Every joined machine is a learner that applies its fleet's state, so its roster is that state:
    // nothing about it lapses while the laptop is SHUT -- the silence bound runs on the steady clock
    // from the process's start, so the wall clock's nine days below decide nothing -- and a grant its
    // fleet's voter signs verifies against what it last applied. A learner itself never leads, so it
    // signs no grant.
    core::platform::ManualClock clock;
    StateLeaseRoster roster { clock };
    auto state = Cluster::ClusterState {};
    state.members.push_back(
        Cluster::ClusterMember { .id = "office",
                                 .raftEndpoint = "office:6680",
                                 .schedulerEndpoint = "office:6674",
                                 .schedulerEndpointHistory = Cluster::SchedulerEndpointHistory::Announced,
                                 .seat = Cluster::MemberSeat::Voter,
                                 .publicKey = TestKeyPair("office").PublicKey() });
    state.members.push_back(
        Cluster::ClusterMember { .id = "laptop",
                                 .raftEndpoint = "",
                                 .schedulerEndpoint = "laptop:6674",
                                 .schedulerEndpointHistory = Cluster::SchedulerEndpointHistory::Announced,
                                 .seat = Cluster::MemberSeat::Learner,
                                 .publicKey = TestKeyPair("laptop").PublicKey() });
    roster.Adopt(state);

    CHECK(roster.KeysOf("office").live == std::optional { TestKeyPair("office").PublicKey() });
    CHECK_FALSE(roster.KeysOf("laptop").live.has_value()); // a learner signs no grant
    CHECK(roster.Read(Noon + std::chrono::days { 9 }).standing == RosterStanding::Current);

    auto const grant = LeaseClaims { .serial = "1",
                                     .endpoint = "laptop:6674",
                                     .fingerprint = "clang-19-x86_64",
                                     .key = "obj-1",
                                     .expiresAt = Noon + 10min,
                                     .clusterId = "c-office",
                                     .epoch = 3,
                                     .signer = {} };
    CHECK(AuthenticateLeaseToken(roster, MintLeaseToken(KeyPairLeaseSigner { "office", TestKeyPair("office") }, grant))
              .has_value());
}

TEST_CASE("A node's roster is the state it applied: no certificate, never lapsing", "[distributed][roster]")
{
    // Before the first state that records a voter's key there is nothing a grant could verify
    // against, which is a fact about this node rather than about any grant.
    core::platform::ManualClock emptyClock;
    StateLeaseRoster empty { emptyClock };
    CHECK(empty.Read(Noon).standing == RosterStanding::Absent);

    Cluster::ClusterState state;
    state.members = { Cluster::ClusterMember { .id = "n1",
                                               .raftEndpoint = "n1:6680",
                                               .schedulerEndpoint = {},
                                               .schedulerEndpointHistory = Cluster::SchedulerEndpointHistory::NeverAnnounced,
                                               .seat = Cluster::MemberSeat::Voter,
                                               .publicKey = TestKeyPair("n1").PublicKey() },
                      Cluster::ClusterMember { .id = "n2",
                                               .raftEndpoint = "n2:6680",
                                               .schedulerEndpoint = {},
                                               .schedulerEndpointHistory = Cluster::SchedulerEndpointHistory::NeverAnnounced,
                                               .seat = Cluster::MemberSeat::Learner,
                                               .publicKey = TestKeyPair("n2").PublicKey() } };
    state.revokedKeys = { Cluster::RevokedKey { .id = "n9", .publicKey = TestKeyPair("n9").PublicKey() } };
    state.rosterVersion = 11;

    core::platform::ManualClock clock;
    StateLeaseRoster roster { clock };
    roster.Adopt(state);
    CHECK(roster.KeysOf("n1").live == TestKeyPair("n1").PublicKey());
    // A learner cannot lead, so it signs no grant.
    CHECK_FALSE(roster.KeysOf("n2").live.has_value());
    CHECK(std::ranges::contains(roster.KeysOf("n1").revoked, TestKeyPair("n9").PublicKey()));
    CHECK(roster.Read(Noon + std::chrono::hours { 10'000 }).standing == RosterStanding::Current);
    CHECK(roster.Summary().version == 11);
    CHECK(roster.Summary().voters == 1);
}

TEST_CASE("A grant signed by a voter the cluster has since forgotten is refused as the removed machine's",
          "[distributed][roster][lease]")
{
    // #1555 asked it of the whole path rather than of a fake: a voter is admitted, signs, is then
    // FORGOTTEN -- `--cluster-forget`'s one act, which removes the record and revokes its key --
    // and a member applying that state names the grant it still mints `SignerRevoked`.
    auto const admitted = [](std::string const& id) {
        return Cluster::Command { .kind = Cluster::CommandKind::AddMember,
                                  .key = id,
                                  .value = id + ":6680",
                                  .schedulerEndpoint = {},
                                  .publicKey = TestKeyPair(id).PublicKey(),
                                  .role = std::nullopt };
    };
    auto const forget = [](Cluster::ClusterState& state, std::string const& id) {
        Cluster::Apply(state,
                       Cluster::Command { .kind = Cluster::CommandKind::Forget,
                                          .key = id,
                                          .value = {},
                                          .schedulerEndpoint = {},
                                          .publicKey = std::nullopt,
                                          .role = std::nullopt });
    };
    auto const grant = LeaseClaims { .serial = "17",
                                     .endpoint = "10.0.0.7:6675",
                                     .fingerprint = "clang-19-x86_64",
                                     .key = "obj-abc",
                                     .expiresAt = Noon + 10min,
                                     .clusterId = "fleet",
                                     .epoch = 7,
                                     .signer = {} };

    Cluster::ClusterState state;
    for (auto const* id: { "n1", "n2", "n3" })
        Cluster::Apply(state, admitted(id));
    core::platform::ManualClock clock;
    StateLeaseRoster roster { clock };
    roster.Adopt(state);

    auto const removed = KeyPairLeaseSigner { "n3", TestKeyPair("n3") };
    auto const token = MintLeaseToken(removed, grant);
    // The premise: while it was a voter, its grant was good.
    REQUIRE(AuthenticateLeaseToken(roster, token).has_value());

    forget(state, "n3");
    REQUIRE(std::ranges::none_of(state.members, [](Cluster::ClusterMember const& m) { return m.id == "n3"; }));
    roster.Adopt(state);

    // Named as the removed machine rather than as a forgery: its signature still verifies, under
    // a key the state keeps whole for exactly this. Unwired, the key is merely UNKNOWN and the
    // refusal reads `Unauthorized` -- the same refusal a stranger earns, which is the confident
    // wrong signal the separate counter exists to avoid.
    auto const refused = AuthenticateLeaseToken(roster, token);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == LeaseRefusalReason::SignerRevoked);

    // And the control: a voter still in good standing signs exactly as before.
    auto const kept = KeyPairLeaseSigner { "n1", TestKeyPair("n1") };
    CHECK(AuthenticateLeaseToken(roster, MintLeaseToken(kept, grant)).has_value());
}

TEST_CASE("A roster answers for every machine it admits, in either seat, and for none it revoked",
          "[distributed][roster][ticket]")
{
    // A machine TICKET is checked against every member of either seat, a GRANT against the voters
    // alone: the wider question must not widen the narrower one.
    Cluster::ClusterState state;
    for (auto const& [id, seat]:
         { std::pair { "v1", Cluster::MemberSeat::Voter }, std::pair { "l1", Cluster::MemberSeat::Learner } })
        state.members.push_back(
            Cluster::ClusterMember { .id = id,
                                     .raftEndpoint = {},
                                     .schedulerEndpoint = {},
                                     .schedulerEndpointHistory = Cluster::SchedulerEndpointHistory::NeverAnnounced,
                                     .seat = seat,
                                     .publicKey = TestKeyPair(id).PublicKey() });
    state.revokedKeys.push_back(Cluster::RevokedKey { .id = "gone", .publicKey = TestKeyPair("gone").PublicKey() });
    core::platform::ManualClock clock;
    StateLeaseRoster applied { clock };
    applied.Adopt(state);

    CHECK(applied.MachineKeysOf("v1").live == TestKeyPair("v1").PublicKey());
    CHECK(applied.MachineKeysOf("l1").live == TestKeyPair("l1").PublicKey());
    CHECK_FALSE(applied.MachineKeysOf("gone").live.has_value());
    CHECK_FALSE(applied.MachineKeysOf("stranger").live.has_value());
    CHECK(std::ranges::contains(applied.MachineKeysOf("stranger").revoked, TestKeyPair("gone").PublicKey()));
    // A GRANT is still a voter's alone.
    CHECK_FALSE(applied.KeysOf("l1").live.has_value());
}

TEST_CASE("A roster that holds nothing answers for no machine", "[distributed][roster][ticket]")
{
    core::platform::ManualClock clock;
    StateLeaseRoster const empty { clock };
    CHECK_FALSE(empty.MachineKeysOf("v1").live.has_value());
    CHECK(empty.MachineKeysOf("v1").revoked.empty());
}

TEST_CASE("A roster no counted leader has refreshed for longer than the bound refuses every grant until one speaks",
          "[distributed][roster][lease][isolation]")
{
    // A worker cut off with a voter its fleet has since forgotten still lists that voter, so its
    // grants would verify for as long as the isolation lasted. Bounded by this node's OWN consensus:
    // past `LeaderSilenceBound` without a leader its applied configuration counts, the roster stands
    // `Isolated` -- a fact about this worker -- and one contact makes it `Current` again.
    core::platform::ManualClock clock;
    StateLeaseRoster roster { clock };
    Cluster::ClusterState state;
    state.members = { Cluster::ClusterMember { .id = "n1",
                                               .raftEndpoint = "n1:6680",
                                               .schedulerEndpoint = {},
                                               .schedulerEndpointHistory = Cluster::SchedulerEndpointHistory::NeverAnnounced,
                                               .seat = Cluster::MemberSeat::Voter,
                                               .publicKey = TestKeyPair("n1").PublicKey() } };
    roster.Adopt(state);

    // The start counts as contact: just under the bound it is honoured...
    clock.advance(LeaderSilenceBound - 1s);
    CHECK(roster.Read(Noon).standing == RosterStanding::Current);
    CHECK_FALSE(roster.Isolated());
    // ...just over, refused.
    clock.advance(2s);
    CHECK(roster.Read(Noon).standing == RosterStanding::Isolated);
    CHECK(roster.Isolated());

    // An OLDER reading moves nothing: two reporters cannot set the record back.
    roster.NoteLeaderReading(LeaderReading { .leads = false, .leader = "n1", .silentFor = LeaderSilenceBound + 1min });
    CHECK(roster.Read(Noon).standing == RosterStanding::Isolated);

    // One heartbeat from a counted leader, and it is honoured again.
    roster.NoteLeaderReading(LeaderReading { .leads = false, .leader = "n1", .silentFor = 0s });
    CHECK(roster.Read(Noon).standing == RosterStanding::Current);

    // The WALL clock decides nothing: an NTP step neither lapses the state nor revives it.
    CHECK(roster.Read(Noon + std::chrono::days { 30 }).standing == RosterStanding::Current);

    // And a roster holding no voter is `Absent` however long the silence: the first fact first.
    core::platform::ManualClock emptyClock;
    StateLeaseRoster empty { emptyClock };
    emptyClock.advance(LeaderSilenceBound + 1h);
    CHECK(empty.Read(Noon).standing == RosterStanding::Absent);
}

TEST_CASE("Only a leader the APPLIED configuration seats as a voter refreshes the bound",
          "[distributed][roster][lease][isolation]")
{
    // The driver's configuration is its ACTIVE one, which an uncommitted entry can move: a member the
    // applied state seats as a LEARNER can promote itself there and send AppendEntries as leader. Its
    // traffic must not keep an isolated worker honouring a forgotten voter's grants, so a reading
    // counts only for a leader the voters THIS roster applied include.
    core::platform::ManualClock clock;
    StateLeaseRoster roster { clock };
    auto const member = [](std::string const& id, Cluster::MemberSeat seat) {
        return Cluster::ClusterMember { .id = id,
                                        .raftEndpoint = id + ":6680",
                                        .schedulerEndpoint = {},
                                        .schedulerEndpointHistory = Cluster::SchedulerEndpointHistory::NeverAnnounced,
                                        .seat = seat,
                                        .publicKey = TestKeyPair(id).PublicKey() };
    };
    Cluster::ClusterState state;
    state.members = { member("n1", Cluster::MemberSeat::Voter), member("n5", Cluster::MemberSeat::Learner) };
    roster.Adopt(state);
    clock.advance(LeaderSilenceBound + 1s);
    REQUIRE(roster.Read(Noon).standing == RosterStanding::Isolated);

    // The applied LEARNER speaking as leader, and a leader the applied state does not hold at all.
    roster.NoteLeaderReading(LeaderReading { .leads = false, .leader = "n5", .silentFor = 0s });
    CHECK(roster.Read(Noon).standing == RosterStanding::Isolated);
    roster.NoteLeaderReading(LeaderReading { .leads = false, .leader = "n9", .silentFor = 0s });
    CHECK(roster.Read(Noon).standing == RosterStanding::Isolated);
    // Nobody named, or nothing heard, is no contact either.
    roster.NoteLeaderReading(LeaderReading { .leads = false, .leader = std::nullopt, .silentFor = 0s });
    roster.NoteLeaderReading(LeaderReading { .leads = false, .leader = "n1", .silentFor = std::nullopt });
    CHECK(roster.Read(Noon).standing == RosterStanding::Isolated);

    // The control: the applied VOTER, and this node leading, each refresh it.
    roster.NoteLeaderReading(LeaderReading { .leads = false, .leader = "n1", .silentFor = 0s });
    CHECK(roster.Read(Noon).standing == RosterStanding::Current);
    clock.advance(LeaderSilenceBound + 1s);
    roster.NoteLeaderReading(LeaderReading { .leads = true, .leader = std::nullopt, .silentFor = 0s });
    CHECK(roster.Read(Noon).standing == RosterStanding::Current);
}

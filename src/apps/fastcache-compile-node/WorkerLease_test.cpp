// SPDX-License-Identifier: Apache-2.0
#include "NodeAnnounce.hpp"
#include "NodeDefaults.hpp"
#include "NodeFormation.hpp"
#include "NodeRoster.hpp"
#include "WorkerLease.hpp"

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Cluster/NodeMode.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/LeaseSigner.hpp>
#include <FastCache/Distributed/LeaseToken.hpp>
#include <FastCache/Distributed/SchedulerService.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <core/platform/Clock.hpp>
#include <tests/LeaseRosterFakes.hpp>
#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;

namespace
{

/// The endpoint the worker under test advertises, and the one a grant must name.
inline constexpr std::string_view ThisWorker = "worker-under-test:6675";

/// Where it ends up once a NAT or a late interface tells it its real address.
///
/// Different in both host and port from `ThisWorker`, so a comparison matching on
/// either half alone cannot pass.
inline constexpr std::string_view MovedWorker = "worker-behind-nat:7700";

/// The fleet it belongs to.
inline constexpr std::string_view ThisCluster = "fleet-a";

/// The term the scheduler in these cases is leading under.
inline constexpr std::uint64_t CurrentTerm = 9;

/// A term it was leading under before an election.
inline constexpr std::uint64_t DeposedTerm = 4;

/// A fixed clock, because what these cases turn on is the TERM a grant names and an
/// expiry that moved per run would make a failure look like a flake.
core::platform::ManualWallClock const LeaseClock { std::chrono::system_clock::time_point {
    std::chrono::seconds { 1704067200 } } };

/// The scheduler that signs every grant here. Its key is derived from its name, constant
/// for the reason the clock is.
/// @return The signer.
[[nodiscard]] Distributed::KeyPairLeaseSigner const& TestSigner()
{
    static auto const signer = Testing::TestLeaseSigner("scheduler");
    return signer;
}

/// The roster a checking worker holds: the scheduler above, as a voter (#178).
/// @return The roster; one for the file, since a validator borrows it and nothing here changes it.
[[nodiscard]] Testing::FixedLeaseRoster const& TestRoster()
{
    static Testing::FixedLeaseRoster const roster { { "scheduler" } };
    return roster;
}

/// A node configured the way `main.cpp` configures one that checks grants: its fleet named.
/// What it checks AGAINST is the roster handed to the factory beside it.
/// @return The configuration.
[[nodiscard]] NodeConfig CheckingConfig()
{
    NodeConfig cfg;
    cfg.clusterId = std::string { ThisCluster };
    return cfg;
}

/// A grant, signed the way a scheduler signs one.
///
/// **The serial is a parameter and it has to be** (#614). A grant is spendable exactly
/// once at a worker, so two calls to this that produced identical bytes would make the
/// second look like a replay -- which is what a real `LeaseTable` never does either:
/// `_nextToken` advances per acquisition, so no two grants a scheduler mints share a
/// serial.
/// @param epoch The scheduler term it is issued under.
/// @param serial What distinguishes this issuance from the last.
/// @param endpoint The worker it is issued FOR. A defaulted parameter rather than a
///        third near-copy of this mint call, which is what the two below already are.
/// @return The token, as a client would present it.
[[nodiscard]] std::string GrantUnder(std::uint64_t epoch,
                                     std::string_view serial = "17",
                                     std::string_view endpoint = ThisWorker)
{
    return Distributed::MintLeaseToken(
        TestSigner(),
        Distributed::LeaseClaims { .serial = std::string { serial },
                                   .endpoint = std::string { endpoint },
                                   .fingerprint = "gcc-13",
                                   .key = "obj-abc",
                                   .expiresAt = LeaseClock.now() + std::chrono::minutes { 10 },
                                   .clusterId = std::string { ThisCluster },
                                   .epoch = epoch,
                                   .signer = {} });
}

/// A grant from ANOTHER fleet, signed by the same voter.
///
/// The shape two sites whose rosters name one machine produce, which is what copying a
/// working state directory or cloning staging from production gives you. The signature
/// verifies; only the fleet differs.
/// @param epoch The scheduler term it is issued under.
/// @param serial What distinguishes this issuance from the last; see `GrantUnder`.
/// @return The token, as a client would present it.
[[nodiscard]] std::string ForeignGrantUnder(std::uint64_t epoch, std::string_view serial = "17")
{
    return Distributed::MintLeaseToken(
        TestSigner(),
        Distributed::LeaseClaims { .serial = std::string { serial },
                                   .endpoint = std::string { ThisWorker },
                                   .fingerprint = "gcc-13",
                                   .key = "obj-abc",
                                   .expiresAt = LeaseClock.now() + std::chrono::minutes { 10 },
                                   .clusterId = "fleet-b",
                                   .epoch = epoch,
                                   .signer = {} });
}

} // namespace

namespace
{
/// Everything `MakeWorkerLeaseValidator` borrows for the life of a worker.
///
/// The lease state is `Distributed::WorkerLeaseState` -- production's own aggregate,
/// not a stand-in for it. This struct is where that grouping was first written, for
/// the reason it still carries: they must all outlive the validator, and a case that
/// declares three of them and forgets the fourth does not fail to compile, it dangles.
/// The sink and the logger join it here because a test wants one of each per case.
struct WorkerState
{
    /// @param reported Where a term going backwards is said; silent unless a case cares.
    explicit WorkerState(
        Distributed::SchedulerTermRegressionNotice reported = Distributed::SchedulerTermRegressionNotice::Silent()):
        lease { std::move(reported) }
    {
    }

    Distributed::WorkerLeaseState lease; ///< Spent grants, learned term, reset notice.
    AtomicMetricsSink metrics;           ///< Where refusals are counted.
    NullLogger logger;                   ///< Where startup lines go.

    /// What this worker advertises, which the validator reads per request rather than
    /// copying (#1279). The production class, not a stand-in: it is in reach here, and a
    /// fake would be a second answer to the one question this struct exists to settle --
    /// what the validator borrows, and who keeps it alive.
    AnnouncedEndpoint advertised { ThisWorker };

    LeaseCheckInForce inForce; ///< Where the factory records the lease check it built.
};
} // namespace

TEST_CASE("A worker that republishes its advertised address verifies grants naming the new one", "[node][lease][advertise]")
{
    // **The WIRING half of #1279, which the validator's own cases cannot see.**
    // `WorkerProtocol_test.cpp` proves `SignedLeaseValidator` follows an
    // `IAdvertisedEndpointSource`; what it cannot prove is that the node's factory hands
    // it the seam rather than a snapshot of the seam. A `MakeWorkerLeaseValidator` that
    // copied the current value would satisfy every case in that file and leave this
    // worker checking grants against the address it booted with -- which is the whole
    // defect, one layer up from where it is testable.
    //
    // So this drives the production path end to end: the real factory, the real
    // `AnnouncedEndpoint`, a roster, and a `Publish` standing in for the heartbeat's
    // re-announce.
    auto const cfg = CheckingConfig();
    WorkerState state;

    auto validator = MakeWorkerLeaseValidator(cfg,
                                              &TestRoster(),
                                              state.advertised,
                                              {},
                                              SocketActivation::No,
                                              LeaseClock,
                                              state.lease,
                                              state.metrics,
                                              state.logger,
                                              state.inForce);
    REQUIRE(validator.has_value());
    // Registered, as every case here but the unregistered one assumes (#401).
    state.lease.fleet.Pin(std::string { ThisCluster });

    // Minted before anything moves, so the only variable between the two halves is what
    // this worker says it answers on. Distinct serials because a grant is spendable
    // exactly once (#614).
    auto const forTheNewAddress = GrantUnder(CurrentTerm, "l-moved", MovedWorker);
    auto const forTheOldAddress = GrantUnder(CurrentTerm, "l-stayed");

    // The control, and it has to come first: at startup the address the process was
    // given is the one that verifies. Without it the case would pass against a factory
    // that built a validator refusing everything.
    CHECK_FALSE((*validator)(forTheOldAddress, "gcc-13").refusal.has_value());

    auto const beforeTheMove = (*validator)(forTheNewAddress, "gcc-13").refusal;
    REQUIRE(beforeTheMove.has_value());
    CHECK(Testing::Unwrap(beforeTheMove).reason == Distributed::LeaseRefusalReason::EndpointMismatch);

    // What the heartbeat does once the configuration's answer moves.
    state.advertised.Publish(std::string { MovedWorker });

    // The SAME token, refused a moment ago, now verifies -- it was never consumed,
    // because a refusal is decided before the spend. A validator holding a copy of the
    // old address answers this exactly as it did above, which is the failure this case
    // exists for.
    CHECK_FALSE((*validator)(forTheNewAddress, "gcc-13").refusal.has_value());
}

TEST_CASE("A worker that has verified no grant still refuses a foreign fleet", "[node][lease][epoch]")
{
    // **#401's acceptance clause, asserted at the production seam.**
    //
    // That ticket said a worker "pins the first identity it authenticates", leaving a
    // window in which one that has verified nothing accepts whichever fleet reaches it
    // first. That was never so: the worker was TOLD its fleet, and `VerifyLeaseToken`
    // compared it before anything else. What has changed is WHERE it is told -- since
    // #401 the identity comes from the REGISTER reply rather than from the node's own cluster id,
    // so this case pins it the way a completed registration round does. The property
    // under test is unchanged: having verified NO grant is not the same as being
    // unpinned, and a worker that has verified nothing still refuses a foreign fleet.
    //
    // Written through `MakeWorkerLeaseValidator` rather than `VerifyLeaseToken`,
    // because the primitive already had a case and the primitive is not where the
    // window would be. What #401 describes could only exist in the PATH -- a worker
    // whose expectation came from somewhere other than its configuration -- so that is
    // where it has to be looked for.
    auto const cfg = CheckingConfig();
    WorkerState state;

    auto validator = MakeWorkerLeaseValidator(cfg,
                                              &TestRoster(),
                                              state.advertised,
                                              {},
                                              SocketActivation::No,
                                              LeaseClock,
                                              state.lease,
                                              state.metrics,
                                              state.logger,
                                              state.inForce);
    REQUIRE(validator.has_value());

    // What a completed registration round does, and the only way this worker learns a
    // fleet since #401. Before this line it refuses everything as `Unregistered`, which
    // is a different case and has its own.
    state.lease.fleet.Pin(std::string { ThisCluster });

    // Nothing has been verified. Asserted, because if the worker had already learnt
    // something this case would be about the term rather than the fleet.
    REQUIRE_FALSE(state.lease.term.Known().has_value());

    // And the foreign fleet is refused anyway -- on the identity, not the term.
    auto const foreign = (*validator)(ForeignGrantUnder(CurrentTerm), "gcc-13").refusal;
    REQUIRE(foreign.has_value());
    CHECK(Testing::Unwrap(foreign).reason == Distributed::LeaseRefusalReason::ClusterMismatch);

    // Still nothing learnt: a refused grant must teach this worker nothing, or anybody
    // holding the wire could move its expectation by sending one.
    CHECK_FALSE(state.lease.term.Known().has_value());

    // **And nothing was SPENT either**, which is the same rule one layer down: a grant
    // refused on a reading of its claims has not been consumed, so a client whose
    // fleet was misconfigured for one job still holds a usable grant. Only a grant
    // that was about to be compiled is spent.
    CHECK(state.lease.spent.Size() == 0);

    // The control, and it is what stops this passing against a validator that refuses
    // everything: this node's OWN fleet is served, first grant and all.
    CHECK_FALSE((*validator)(GrantUnder(CurrentTerm), "gcc-13").refusal.has_value());
}

TEST_CASE("The production factory wires the spend and the term through", "[node][lease][epoch][replay]")
{
    // **The wiring, and it is the acceptance clause for #421 and again for #614.**
    //
    // `WorkerProtocol_test` and `LeaseToken_test` cover each half: that a grant is
    // spendable once, that a term is adopted in either direction, that a reset is
    // said. Neither says anything about the PATH between them. Drop the `spent` or
    // `term` argument `MakeWorkerLeaseValidator` forwards and every one of those cases
    // stays green while the production worker enforces nothing -- `PurgeExpired`
    // exactly: correct, tested, and reachable from nothing.
    //
    // So this drives the factory `main.cpp` actually calls, with a roster, and asserts the
    // whole chain in one case.
    auto const cfg = CheckingConfig();

    // Recording rather than silent, because the notice's WIRING is the half no unit test
    // of the notice itself can see: a `SchedulerTermRegressionNotice` that works perfectly and
    // is never reached is the defect it was written to fix (#614).
    std::vector<std::string> said;
    WorkerState state { Distributed::SchedulerTermRegressionNotice {
        [&said](std::string_view line) { said.emplace_back(line); } } };

    auto validator = MakeWorkerLeaseValidator(cfg,
                                              &TestRoster(),
                                              state.advertised,
                                              {},
                                              SocketActivation::No,
                                              LeaseClock,
                                              state.lease,
                                              state.metrics,
                                              state.logger,
                                              state.inForce);
    REQUIRE(validator.has_value());
    // Registered, as every case here but the unregistered one assumes (#401).
    state.lease.fleet.Pin(std::string { ThisCluster });

    // Nothing learned yet, so this is honoured -- and honouring it is what teaches the
    // term and spends the grant. All three are asserted: without the first, the case
    // would pass against a validator that refuses everything; without the others,
    // against one that accepts everything and enforces nothing.
    auto const first = GrantUnder(CurrentTerm, "l1");
    CHECK_FALSE((*validator)(first, "gcc-13").refusal.has_value());
    CHECK(state.lease.term.Known() == std::optional<std::uint64_t> { CurrentTerm });
    CHECK(state.lease.spent.Size() == 1);
    CHECK(said.empty());

    // **The same grant again is a replay**, refused by name and counted under its own
    // row. This is the check that used to not exist at all: the grant authenticated,
    // named this worker and this toolchain, and had not expired, so it was served
    // every time it arrived.
    auto const replay = (*validator)(first, "gcc-13").refusal;
    REQUIRE(replay.has_value());
    CHECK(Testing::Unwrap(replay).reason == Distributed::LeaseRefusalReason::Replayed);

    // The COUNTER is not read here, and that is the contract rather than a gap: a
    // refusal on this surface is counted by `WorkerProtocol::Compile`, which converts
    // one `LeaseRefusalTable` row into the wire code and the counter together. So the
    // row is what this asserts -- `WorkerProtocol_test` asserts the counter actually
    // rising, at the layer that raises it.
    CHECK(Distributed::DescribeLeaseRefusal(Distributed::LeaseRefusalReason::Replayed).workerCounter
          == IMetricsSink::Counter::WorkerJobsRefusedLeaseReplayed);
    CHECK(state.metrics.Read(IMetricsSink::Counter::WorkerJobsRefusedLeaseReplayed) == 0);

    // **And a legitimate scheduler reset is ADOPTED**, which is the property #614
    // exists for: before it, this grant was refused and every honest grant after it
    // was too, until the process restarted. The fleet keeps working.
    auto const afterReset = GrantUnder(DeposedTerm, "l1-again");
    CHECK_FALSE((*validator)(afterReset, "gcc-13").refusal.has_value());
    CHECK(state.lease.term.Known() == std::optional<std::uint64_t> { DeposedTerm });
    CHECK(state.metrics.Read(IMetricsSink::Counter::WorkerSchedulerTermRegressions) == 1);

    // **And the worker said so.** Without this the notice could be correct in isolation
    // and reached by nothing, which is exactly the shape of a guard nothing constructs.
    // Asserted on the CONTENT as well as the count: the line's whole value is that it
    // carries both terms and says what it did about them.
    REQUIRE(said.size() == 1);
    CHECK(said.front().contains(std::to_string(CurrentTerm)));
    CHECK(said.front().contains(std::to_string(DeposedTerm)));
    // Names BOTH causes rather than asserting the one it cannot know; the wording's own
    // case is in `LeaseToken_test`.
    CHECK(said.front().contains("leadership change"));

    // Once per reset, not once per grant: the fleet is now steady at the lower term and
    // every compile learns it again.
    CHECK_FALSE((*validator)(GrantUnder(DeposedTerm, "l2"), "gcc-13").refusal.has_value());
    CHECK(said.size() == 1);
    CHECK(state.metrics.Read(IMetricsSink::Counter::WorkerSchedulerTermRegressions) == 1);
}

TEST_CASE("A node with no roster builds a validator that learns and spends nothing", "[node][lease][epoch]")
{
    // The other production path through the same factory, asserted because the term and
    // the spent set are taken on BOTH and a reader should not have to guess whether the
    // unchecked one quietly uses them. It refuses nothing and remembers nothing: a node
    // admitting only its own machine has no roster to check a signature against, so it has
    // no authentic term to believe and no authentic grant to spend.
    //
    // The second half matters on its own: a spend here would make such a node refuse the
    // second compile of any TU whose token bytes repeated.
    NodeConfig cfg;
    WorkerState state;
    CapturingLogger logger;

    auto validator = MakeWorkerLeaseValidator(cfg,
                                              nullptr,
                                              state.advertised,
                                              {},
                                              SocketActivation::No,
                                              LeaseClock,
                                              state.lease,
                                              state.metrics,
                                              logger,
                                              state.inForce);
    REQUIRE(validator.has_value());
    // What the reload guard reads: the factory recorded the check it built (review I-2b).
    CHECK(state.inForce.Current() == BuiltLeaseCheck::Unchecked);
    // Said once, and in words that are TRUE on this tree: no consensus is no roster, and no flag
    // names one -- `--voter-key`, which once did, anchors nothing (#178).
    auto const lines = logger.Snapshot();
    REQUIRE(lines.size() == 1);
    CHECK(lines.front().level == LogLevel::Warn);
    CHECK(lines.front().message.contains("runs no consensus and so keeps no roster"));
    CHECK_FALSE(lines.front().message.contains("--voter-key"));
    // Registered, as every case here but the unregistered one assumes (#401).
    state.lease.fleet.Pin(std::string { ThisCluster });

    CHECK_FALSE((*validator)(GrantUnder(CurrentTerm), "gcc-13").refusal.has_value());
    CHECK_FALSE((*validator)(GrantUnder(CurrentTerm), "gcc-13").refusal.has_value());
    CHECK_FALSE(state.lease.term.Known().has_value());
    CHECK(state.lease.spent.Size() == 0);
}

TEST_CASE("A socket-activated worker that admits remote peers and holds no roster is refused", "[node][lease][roster]")
{
    // The backstop the startup table cannot be (#282, #178): under socket activation the
    // unit chose the address, so `--bind` describes nothing, and a node with no roster must
    // not build a validator that refuses nothing. The refusal names the remedy.
    NodeConfig cfg;
    cfg.fleetOpen = true;
    WorkerState state;

    auto const refused = MakeWorkerLeaseValidator(cfg,
                                                  nullptr,
                                                  state.advertised,
                                                  {},
                                                  SocketActivation::Yes,
                                                  LeaseClock,
                                                  state.lease,
                                                  state.metrics,
                                                  state.logger,
                                                  state.inForce);
    REQUIRE_FALSE(refused.has_value());
    // A remedy an operator can follow: the only roster is consensus's. Naming `--voter-key`, which
    // anchors nothing since the certified roster retired, would get the same refusal again.
    CHECK(refused.error().contains("run consensus (--listen-raft)"));
    CHECK_FALSE(refused.error().contains("--voter-key"));
    // Refused before anything was built, so nothing is recorded for the reload guard to read.
    CHECK(state.inForce.Current() == BuiltLeaseCheck::None);

    // The control: the same node HOLDING a roster builds its checking validator, and says what it
    // checks against -- the state its consensus applies, the one roster there is.
    CapturingLogger logger;
    CHECK(MakeWorkerLeaseValidator(cfg,
                                   &TestRoster(),
                                   state.advertised,
                                   {},
                                   SocketActivation::Yes,
                                   LeaseClock,
                                   state.lease,
                                   state.metrics,
                                   logger,
                                   state.inForce)
              .has_value());
    auto const lines = logger.Snapshot();
    REQUIRE(lines.size() == 1);
    CHECK(lines.front().message.contains("against the state this node's consensus applies"));
    CHECK_FALSE(lines.front().message.contains("certify"));
    CHECK(state.inForce.Current() == BuiltLeaseCheck::Signed);
}

TEST_CASE("A socket-activated worker with no roster is opened only by --fleet-open, and refused then",
          "[node][lease][admission]")
{
    // No roster means no proof and no ticket can admit anybody, so the factory asks
    // `AdmitsRemotePeers` with `Absent`: only `--fleet-open` or a fleet the formation record
    // puts it in widens such a node. Asked with `Unknown` instead, a state directory -- which
    // every worker keeps -- would count as a key route and refuse every
    // activated worker the directory turned out to hold no roster for, although nobody remote
    // is admitted there.
    NodeConfig cfg;
    cfg.clusterDir = "cluster";
    WorkerState state;

    auto validator = MakeWorkerLeaseValidator(cfg,
                                              nullptr,
                                              state.advertised,
                                              {},
                                              SocketActivation::Yes,
                                              LeaseClock,
                                              state.lease,
                                              state.metrics,
                                              state.logger,
                                              state.inForce);
    REQUIRE(validator.has_value());
    CHECK(state.inForce.Current() == BuiltLeaseCheck::Unchecked);
    // The unchecked validator: nobody remote is admitted, so it refuses nothing and spends
    // nothing.
    state.lease.fleet.Pin(std::string { ThisCluster });
    CHECK_FALSE((*validator)(GrantUnder(CurrentTerm), "gcc-13").refusal.has_value());
    CHECK(state.lease.spent.Size() == 0);

    // And `--fleet-open` admits every caller, so the same node is refused, naming the remedy.
    cfg.fleetOpen = true;
    WorkerState opened;
    auto const refused = MakeWorkerLeaseValidator(cfg,
                                                  nullptr,
                                                  opened.advertised,
                                                  {},
                                                  SocketActivation::Yes,
                                                  LeaseClock,
                                                  opened.lease,
                                                  opened.metrics,
                                                  opened.logger,
                                                  opened.inForce);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().contains("--listen-raft"));
}

TEST_CASE("A learner verifies the grant its fleet's leader signed against the state it applied", "[node][lease][formation]")
{
    // The cross-machine half of what `dist-compile-e2e` used to check with two processes and a typed
    // voter key: a machine that joined a fleet is leased out by that fleet's LEADER, so the
    // grant it is handed was signed on another machine. It holds no key anybody typed -- it
    // verifies against the cluster state its consensus applied, through the production roster
    // (`NodeRoster`) and the production factory. A stranger's grant is refused as `Unauthorized`.
    auto cfg = CheckingConfig();
    cfg.formation = NodeFormationView { .mode = Cluster::NodeMode::Learner,
                                        .clusterId = std::string { ThisCluster },
                                        .createdAtUnixSeconds = 0,
                                        .foundedHere = false,
                                        .fleetMembers = {},
                                        .fleetSchedulers = { "scheduler:6674" } };
    REQUIRE(RunsConsensus(cfg));
    REQUIRE_FALSE(ServesScheduler(cfg));

    core::platform::ManualClock rosterClock;
    auto built = NodeRoster::Build(cfg, rosterClock, nullptr);
    REQUIRE(built.has_value());
    auto& roster = *Testing::Unwrap(built);
    REQUIRE(roster.Lease() != nullptr);

    // What the learner's consensus applied: the leader, a voter, under its own key.
    Cluster::ClusterState applied;
    applied.members = { Cluster::ClusterMember { .id = "scheduler",
                                                 .raftEndpoint = "scheduler:6680",
                                                 .schedulerEndpoint = "scheduler:6674",
                                                 .schedulerEndpointHistory = Cluster::SchedulerEndpointHistory::Announced,
                                                 .seat = Cluster::MemberSeat::Voter,
                                                 .publicKey = Testing::TestKeyPair("scheduler").PublicKey() } };
    roster.Applied(applied);

    WorkerState state;
    auto validator = MakeWorkerLeaseValidator(cfg,
                                              roster.Lease(),
                                              state.advertised,
                                              {},
                                              SocketActivation::No,
                                              LeaseClock,
                                              state.lease,
                                              state.metrics,
                                              state.logger,
                                              state.inForce);
    REQUIRE(validator.has_value());
    state.lease.fleet.Pin(std::string { ThisCluster });

    // The leader's grant, naming this worker, verifies.
    CHECK_FALSE((*validator)(GrantUnder(CurrentTerm, "from-the-leader"), "gcc-13").refusal.has_value());

    // A grant from a machine the applied state holds no voter key for does not.
    auto const stranger =
        Distributed::MintLeaseToken(Testing::TestLeaseSigner("stranger"),
                                    Distributed::LeaseClaims { .serial = "from-a-stranger",
                                                               .endpoint = std::string { ThisWorker },
                                                               .fingerprint = "gcc-13",
                                                               .key = "obj-abc",
                                                               .expiresAt = LeaseClock.now() + std::chrono::minutes { 10 },
                                                               .clusterId = std::string { ThisCluster },
                                                               .epoch = CurrentTerm,
                                                               .signer = {} });
    auto const refused = (*validator)(stranger, "gcc-13").refusal;
    REQUIRE(refused.has_value());
    CHECK(Testing::Unwrap(refused).reason == Distributed::LeaseRefusalReason::Unauthorized);
}

TEST_CASE("A node whose name reaches only itself grants its own worker a lease that it checks against the state it applies",
          "[node][lease][formation][defaults]")
{
    // A fleet of its own on loopback (`ConsensusConfinedToThisMachine`): its scheduler -- the
    // production service, signing with this node's identity -- leases its own worker at the loopback
    // endpoint the worker advertises, and the worker verifies that grant through the production roster
    // and factory. Local dispatch works on such a machine; the checking path is the one that runs.
    core::platform::ManualClock clock;
    core::platform::ManualWallClock wall { LeaseClock.now() };
    auto cfg = CheckingConfig();
    cfg.nodeId = "scheduler";
    cfg.toolchains = { "/usr/bin/g++" };
    cfg.formation = NodeFormationView { .mode = Cluster::NodeMode::Solitary,
                                        .clusterId = std::string { ThisCluster },
                                        .createdAtUnixSeconds = 0,
                                        .foundedHere = true,
                                        .fleetMembers = {},
                                        .fleetSchedulers = {} };
    ApplyHostNames(cfg, NodeHostNames { .fqdn = {}, .dnsSuffix = {}, .withheld = "localhost" });
    REQUIRE(ConsensusConfinedToThisMachine(cfg));
    REQUIRE(ServesScheduler(cfg));
    REQUIRE(SchedulersOf(cfg, AsConfigured) == std::vector<std::string> { "127.0.0.1:6674" });
    auto const advertised = AdvertisedEndpoint(cfg);
    REQUIRE(advertised == "127.0.0.1:6674");

    // Its scheduler: the worker registers where `SchedulersOf` says, from this machine, and leases.
    AtomicMetricsSink schedulerMetrics;
    NullLogger schedulerLogger;
    auto const signer = Testing::TestLeaseSigner("scheduler");
    Distributed::SchedulerService scheduler { clock, wall, schedulerMetrics, schedulerLogger, signer, ThisCluster };
    scheduler.SetRole(Distributed::SchedulerRole::Leader, {}, Distributed::StandaloneSchedulerTerm);
    auto const local = Distributed::CallerContext { .membership = Distributed::Membership::Member, .peerId = "127.0.0.1" };
    auto const registered = scheduler.Register(
        local,
        Distributed::WorkerRegistration { .fingerprint = "gcc-13", .endpoint = advertised, .slots = 1, .codecs = {} });
    REQUIRE(registered.status == CompileCacheWire::Status::Ok);
    auto const granted = scheduler.Lease(
        local, CompileCacheWire::LeaseRequest { .fingerprint = "gcc-13", .key = "obj-abc", .acceptedCodecs = {} });
    REQUIRE(granted.status == CompileCacheWire::Status::Ok);
    auto const grant = CompileCacheWire::DecodeLeaseGrant(granted.payload);
    REQUIRE(grant.has_value());
    CHECK(CompileCacheWire::AsStringView(Testing::Unwrap(grant).endpoint) == advertised);

    // Its worker: the roster is the state its own consensus applies, this node its one voter.
    core::platform::ManualClock rosterClock;
    auto built = NodeRoster::Build(cfg, rosterClock, nullptr);
    REQUIRE(built.has_value());
    auto& roster = *Testing::Unwrap(built);
    REQUIRE(roster.Lease() != nullptr);
    Cluster::ClusterState applied;
    applied.members = { Cluster::ClusterMember { .id = "scheduler",
                                                 .raftEndpoint = ConsensusDialAddressOf(cfg).value_or(std::string {}),
                                                 .schedulerEndpoint = advertised,
                                                 .schedulerEndpointHistory = Cluster::SchedulerEndpointHistory::Announced,
                                                 .seat = Cluster::MemberSeat::Voter,
                                                 .publicKey = Testing::TestKeyPair("scheduler").PublicKey() } };
    roster.Applied(applied);

    Distributed::WorkerLeaseState lease { Distributed::SchedulerTermRegressionNotice::Silent() };
    AtomicMetricsSink workerMetrics;
    CapturingLogger workerLogger;
    AnnouncedEndpoint announced { advertised };
    LeaseCheckInForce inForce;
    auto validator = MakeWorkerLeaseValidator(
        cfg, roster.Lease(), announced, {}, SocketActivation::No, wall, lease, workerMetrics, workerLogger, inForce);
    REQUIRE(validator.has_value());
    lease.fleet.Pin(std::string { ThisCluster });
    auto const said = [&workerLogger](std::string_view phrase) {
        return std::ranges::any_of(workerLogger.Snapshot(), [phrase](CapturingLogger::Record const& record) {
            return record.message.contains(phrase);
        });
    };
    CHECK(said("verifying lease signatures against the state this node's consensus applies"));
    CHECK_FALSE(said("compiling WITHOUT verifying"));

    auto const decision = (*validator)(CompileCacheWire::AsStringView(Testing::Unwrap(grant).leaseToken), "gcc-13");
    CHECK_FALSE(decision.refusal.has_value());
}

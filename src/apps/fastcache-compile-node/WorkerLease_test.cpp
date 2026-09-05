// SPDX-License-Identifier: Apache-2.0
#include "WorkerLease.hpp"

#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/LeaseToken.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <tests/ScratchPath.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;

namespace
{

/// The endpoint the worker under test advertises, and the one a grant must name.
inline constexpr std::string_view ThisWorker = "worker-under-test:6675";

/// The fleet it belongs to.
inline constexpr std::string_view ThisCluster = "fleet-a";

/// The term the scheduler in these cases is leading under.
inline constexpr std::uint64_t CurrentTerm = 9;

/// A term it was leading under before an election.
inline constexpr std::uint64_t DeposedTerm = 4;

/// A fixed clock, because what these cases turn on is the TERM a grant names and an
/// expiry that moved per run would make a failure look like a flake.
ManualWallClock const LeaseClock { std::chrono::system_clock::time_point { std::chrono::seconds { 1704067200 } } };

/// The key both actors share. Constant for the reason the clock is.
/// @return Thirty-two arbitrary bytes.
[[nodiscard]] std::vector<std::byte> TestClusterKey()
{
    return std::vector<std::byte>(32, std::byte { 0x5A });
}

/// A node configured the way `main.cpp` configures one that checks grants.
/// @param keyFile Where the cluster key was written.
/// @return The configuration.
[[nodiscard]] NodeConfig ConfigWithKey(std::filesystem::path const& keyFile)
{
    NodeConfig cfg;
    cfg.clusterKeyFile = keyFile.string();
    cfg.clusterId = std::string { ThisCluster };
    return cfg;
}

/// Write the cluster key where `ReadClusterKey` will find it.
/// @param keyFile Where to write it.
void WriteClusterKey(std::filesystem::path const& keyFile)
{
    auto const key = TestClusterKey();
    std::ofstream out { keyFile, std::ios::binary };
    out.write(reinterpret_cast<char const*>(key.data()), static_cast<std::streamsize>(key.size()));
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
/// @return The token, as a client would present it.
[[nodiscard]] std::string GrantUnder(std::uint64_t epoch, std::string_view serial = "17")
{
    return Distributed::MintLeaseToken(
        TestClusterKey(),
        Distributed::LeaseClaims { .serial = std::string { serial },
                                   .endpoint = std::string { ThisWorker },
                                   .fingerprint = "gcc-13",
                                   .key = "obj-abc",
                                   .expiresAt = LeaseClock.Now() + std::chrono::minutes { 10 },
                                   .clusterId = std::string { ThisCluster },
                                   .epoch = epoch });
}

/// A grant from ANOTHER fleet, signed with the same key.
///
/// The shape two sites provisioned from one `--cluster-key-file` produce, which is
/// what copying a working configuration or cloning staging from production gives you.
/// The MAC verifies; only the fleet differs.
/// @param epoch The scheduler term it is issued under.
/// @param serial What distinguishes this issuance from the last; see `GrantUnder`.
/// @return The token, as a client would present it.
[[nodiscard]] std::string ForeignGrantUnder(std::uint64_t epoch, std::string_view serial = "17")
{
    return Distributed::MintLeaseToken(
        TestClusterKey(),
        Distributed::LeaseClaims { .serial = std::string { serial },
                                   .endpoint = std::string { ThisWorker },
                                   .fingerprint = "gcc-13",
                                   .key = "obj-abc",
                                   .expiresAt = LeaseClock.Now() + std::chrono::minutes { 10 },
                                   .clusterId = "fleet-b",
                                   .epoch = epoch });
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
    explicit WorkerState(Distributed::SchedulerTermResetNotice reported = Distributed::SchedulerTermResetNotice::Silent()):
        lease { std::move(reported) }
    {
    }

    Distributed::WorkerLeaseState lease; ///< Spent grants, learned term, reset notice.
    AtomicMetricsSink metrics;           ///< Where refusals are counted.
    NullLogger logger;                   ///< Where startup lines go.
};
} // namespace

TEST_CASE("A worker that has verified no grant still refuses a foreign fleet", "[node][lease][epoch]")
{
    // **#401's acceptance clause, asserted at the production seam.**
    //
    // That ticket says a worker "pins the first identity it authenticates", leaving a
    // window in which one that has verified nothing accepts whichever fleet reaches it
    // first. It is not so at this ref, and this case is what says so rather than a
    // reading of the code: the worker is TOLD its fleet by `--cluster-id`, that value
    // reaches `LeaseExpectation::clusterId` through the factory `main.cpp` calls, and
    // `VerifyLeaseToken` compares it before it looks at anything else.
    //
    // Written through `MakeWorkerLeaseValidator` rather than `VerifyLeaseToken`,
    // because the primitive already had a case and the primitive is not where the
    // window would be. What #401 describes could only exist in the PATH -- a worker
    // whose expectation came from somewhere other than its configuration -- so that is
    // where it has to be looked for.
    Testing::ScratchDirectory const scratch { "fc-worker-lease-fleet" };
    auto const keyFile = scratch.Path() / "cluster.key";
    WriteClusterKey(keyFile);

    auto const cfg = ConfigWithKey(keyFile);
    WorkerState state;

    auto validator = MakeWorkerLeaseValidator(
        cfg, ThisWorker, SocketActivation::No, LeaseClock, state.lease, state.metrics, state.logger);
    REQUIRE(validator.has_value());

    // Nothing has been verified. Asserted, because if the worker had already learnt
    // something this case would be about the term rather than the fleet.
    REQUIRE_FALSE(state.lease.term.Known().has_value());

    // And the foreign fleet is refused anyway -- on the identity, not the term.
    auto const foreign = (*validator)(ForeignGrantUnder(CurrentTerm), "gcc-13");
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
    CHECK_FALSE((*validator)(GrantUnder(CurrentTerm), "gcc-13").has_value());
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
    // So this drives the factory `main.cpp` actually calls, with a real key file, and
    // asserts the whole chain in one case.
    Testing::ScratchDirectory const scratch { "fc-worker-lease" };
    auto const keyFile = scratch.Path() / "cluster.key";
    WriteClusterKey(keyFile);

    auto const cfg = ConfigWithKey(keyFile);

    // Recording rather than silent, because the notice's WIRING is the half no unit test
    // of the notice itself can see: a `SchedulerTermResetNotice` that works perfectly and
    // is never reached is the defect it was written to fix (#614).
    std::vector<std::string> said;
    WorkerState state { Distributed::SchedulerTermResetNotice {
        [&said](std::string_view line) { said.emplace_back(line); } } };

    auto validator = MakeWorkerLeaseValidator(
        cfg, ThisWorker, SocketActivation::No, LeaseClock, state.lease, state.metrics, state.logger);
    REQUIRE(validator.has_value());

    // Nothing learned yet, so this is honoured -- and honouring it is what teaches the
    // term and spends the grant. All three are asserted: without the first, the case
    // would pass against a validator that refuses everything; without the others,
    // against one that accepts everything and enforces nothing.
    auto const first = GrantUnder(CurrentTerm, "l1");
    CHECK_FALSE((*validator)(first, "gcc-13").has_value());
    CHECK(state.lease.term.Known() == std::optional<std::uint64_t> { CurrentTerm });
    CHECK(state.lease.spent.Size() == 1);
    CHECK(said.empty());

    // **The same grant again is a replay**, refused by name and counted under its own
    // row. This is the check that used to not exist at all: the grant authenticated,
    // named this worker and this toolchain, and had not expired, so it was served
    // every time it arrived.
    auto const replay = (*validator)(first, "gcc-13");
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
    CHECK_FALSE((*validator)(afterReset, "gcc-13").has_value());
    CHECK(state.lease.term.Known() == std::optional<std::uint64_t> { DeposedTerm });
    CHECK(state.metrics.Read(IMetricsSink::Counter::WorkerSchedulerTermResets) == 1);

    // **And the worker said so.** Without this the notice could be correct in isolation
    // and reached by nothing, which is exactly the shape of a guard nothing constructs.
    // Asserted on the CONTENT as well as the count: the line's whole value is that it
    // carries both terms and says what it did about them.
    REQUIRE(said.size() == 1);
    CHECK(said.front().contains(std::to_string(CurrentTerm)));
    CHECK(said.front().contains(std::to_string(DeposedTerm)));
    CHECK(said.front().contains("Adopting"));

    // Once per reset, not once per grant: the fleet is now steady at the lower term and
    // every compile learns it again.
    CHECK_FALSE((*validator)(GrantUnder(DeposedTerm, "l2"), "gcc-13").has_value());
    CHECK(said.size() == 1);
    CHECK(state.metrics.Read(IMetricsSink::Counter::WorkerSchedulerTermResets) == 1);
}

TEST_CASE("A node with no cluster key builds a validator that learns and spends nothing", "[node][lease][epoch]")
{
    // The other production path through the same factory, asserted because the term and
    // the spent set are taken on BOTH and a reader should not have to guess whether the
    // unchecked one quietly uses them. It refuses nothing and remembers nothing: a node
    // admitting only its own machine has no signature to check, so it has no authentic
    // term to believe and no authentic grant to spend.
    //
    // The second half matters on its own: a spend here would make a keyless node refuse
    // the second compile of any TU whose token bytes repeated, which for an unsigned
    // grant is a bare `LeaseTable` serial that restarts at 1 with the scheduler.
    NodeConfig cfg;
    WorkerState state;

    auto validator = MakeWorkerLeaseValidator(
        cfg, ThisWorker, SocketActivation::No, LeaseClock, state.lease, state.metrics, state.logger);
    REQUIRE(validator.has_value());

    CHECK_FALSE((*validator)(GrantUnder(CurrentTerm), "gcc-13").has_value());
    CHECK_FALSE((*validator)(GrantUnder(CurrentTerm), "gcc-13").has_value());
    CHECK_FALSE(state.lease.term.Known().has_value());
    CHECK(state.lease.spent.Size() == 0);
}

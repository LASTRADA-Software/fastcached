// SPDX-License-Identifier: Apache-2.0
#include "WorkerLease.hpp"

#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/LeaseToken.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <fstream>
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
/// @param epoch The scheduler term it is issued under.
/// @return The token, as a client would present it.
[[nodiscard]] std::string GrantUnder(std::uint64_t epoch)
{
    return Distributed::MintLeaseToken(
        TestClusterKey(),
        Distributed::LeaseClaims { .serial = "17",
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
/// @return The token, as a client would present it.
[[nodiscard]] std::string ForeignGrantUnder(std::uint64_t epoch)
{
    return Distributed::MintLeaseToken(
        TestClusterKey(),
        Distributed::LeaseClaims { .serial = "17",
                                   .endpoint = std::string { ThisWorker },
                                   .fingerprint = "gcc-13",
                                   .key = "obj-abc",
                                   .expiresAt = LeaseClock.Now() + std::chrono::minutes { 10 },
                                   .clusterId = "fleet-b",
                                   .epoch = epoch });
}

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
    Distributed::KnownSchedulerTerm term;
    NullLogger logger;

    // This case is about the fleet check and says nothing about the epoch notice, so
    // the notice is inert here rather than absent -- the parameter is a reference and
    // there is no "no notice" to pass.
    Distributed::LeaseEpochNotice epochNotice { [](std::string_view) {} };

    auto validator = MakeWorkerLeaseValidator(cfg, ThisWorker, SocketActivation::No, LeaseClock, term, epochNotice, logger);
    REQUIRE(validator.has_value());

    // Nothing has been verified: the epoch check is `NotKnownHere` and accepts any
    // term. Asserted, because if the worker had already learnt something this case
    // would be about the epoch rather than the fleet.
    REQUIRE_FALSE(term.Check().Checked());

    // And the foreign fleet is refused anyway -- on the identity, not the term.
    auto const foreign = (*validator)(ForeignGrantUnder(CurrentTerm), "gcc-13");
    REQUIRE(foreign.has_value());
    CHECK(Testing::Unwrap(foreign).reason == Distributed::LeaseRefusalReason::ClusterMismatch);

    // Still nothing learnt: a refused grant must teach this worker nothing, or anybody
    // holding the wire could move its expectation by sending one.
    CHECK_FALSE(term.Check().Checked());

    // The control, and it is what stops this passing against a validator that refuses
    // everything: this node's OWN fleet is served, first grant and all.
    CHECK_FALSE((*validator)(GrantUnder(CurrentTerm), "gcc-13").has_value());
}

TEST_CASE("The production factory wires the term through, grant to refusal", "[node][lease][epoch]")
{
    // **The wiring, and it is the acceptance clause for #421.**
    //
    // `WorkerProtocol_test` covers each half: that `LeaseEpochCheck` compares the way
    // it should, and that a validator handed a `KnownSchedulerTerm` taught by hand
    // refuses a stale grant. Neither says anything about the PATH between them. Drop
    // the `term` argument `MakeWorkerLeaseValidator` forwards and every one of those
    // cases stays green while `..._lease_stale_epoch_total` returns to the permanent
    // zero this ticket exists to fix -- `PurgeExpired` exactly: correct, tested, and
    // reachable from nothing.
    //
    // So this drives the factory `main.cpp` actually calls, with a real key file, and
    // asserts the whole chain in one case.
    Testing::ScratchDirectory const scratch { "fc-worker-lease" };
    auto const keyFile = scratch.Path() / "cluster.key";
    WriteClusterKey(keyFile);

    auto const cfg = ConfigWithKey(keyFile);
    Distributed::KnownSchedulerTerm term;
    NullLogger logger;

    // Recording rather than silent, because the notice's WIRING is the half no unit
    // test of the notice itself can see: a `LeaseEpochNotice` that works perfectly and
    // is never reached is the defect it was written to fix (#614). This case already
    // drives the factory `main.cpp` calls, with a real key file and a real refusal, so
    // it is the one place the whole chain is observable.
    std::vector<std::string> said;
    Distributed::LeaseEpochNotice epochNotice { [&said](std::string_view line) { said.emplace_back(line); } };

    auto validator = MakeWorkerLeaseValidator(cfg, ThisWorker, SocketActivation::No, LeaseClock, term, epochNotice, logger);
    REQUIRE(validator.has_value());

    // Nothing learned yet, so this is honoured -- and honouring it is what teaches the
    // term. Both halves are asserted: without the first, the case would pass against a
    // validator that refuses everything; without the second, against one that accepts
    // everything.
    CHECK_FALSE((*validator)(GrantUnder(CurrentTerm), "gcc-13").has_value());
    CHECK(term.Check().Checked());
    CHECK(term.Check().Expected() == CurrentTerm);

    // An accepted grant leaves nothing to report -- and clears the latch, which is what
    // lets a reset AFTER a period of healthy service still be reported.
    CHECK(said.empty());

    auto const stale = (*validator)(GrantUnder(DeposedTerm), "gcc-13");
    REQUIRE(stale.has_value());
    CHECK(Testing::Unwrap(stale).reason == Distributed::LeaseRefusalReason::EpochMismatch);

    // **And the worker said so.** Without this the notice could be correct in isolation
    // and reached by nothing, which is exactly the shape of a guard nothing constructs.
    // Asserted on the CONTENT as well as the count: the line's whole value is that it
    // carries both terms, so an operator reading it knows the scheduler went backwards
    // rather than that the cluster is unstable.
    REQUIRE(said.size() == 1);
    CHECK(said.front().contains(std::to_string(DeposedTerm)));
    CHECK(said.front().contains(std::to_string(CurrentTerm)));
    CHECK(said.front().contains("restart"));

    // Still once, however many more arrive -- a worker refusing every compile must not
    // fill its log with the same paragraph.
    CHECK((*validator)(GrantUnder(DeposedTerm), "gcc-13").has_value());
    CHECK(said.size() == 1);
}

TEST_CASE("A node with no cluster key builds a validator that learns nothing", "[node][lease][epoch]")
{
    // The other production path through the same factory, asserted because the term
    // argument is taken on BOTH and a reader should not have to guess whether the
    // unchecked one quietly learns. It refuses nothing and teaches nothing: a node
    // admitting only its own machine has no signature to check and therefore no
    // authentic term to believe.
    NodeConfig cfg;
    Distributed::KnownSchedulerTerm term;
    Distributed::LeaseEpochNotice epochNotice { Distributed::LeaseEpochNotice::Silent() };
    NullLogger logger;

    auto validator = MakeWorkerLeaseValidator(cfg, ThisWorker, SocketActivation::No, LeaseClock, term, epochNotice, logger);
    REQUIRE(validator.has_value());

    CHECK_FALSE((*validator)(GrantUnder(CurrentTerm), "gcc-13").has_value());
    CHECK_FALSE(term.Check().Checked());
}

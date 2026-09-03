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

} // namespace

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

    auto validator = MakeWorkerLeaseValidator(cfg, ThisWorker, SocketActivation::No, LeaseClock, term, logger);
    REQUIRE(validator.has_value());

    // Nothing learned yet, so this is honoured -- and honouring it is what teaches the
    // term. Both halves are asserted: without the first, the case would pass against a
    // validator that refuses everything; without the second, against one that accepts
    // everything.
    CHECK_FALSE((*validator)(GrantUnder(CurrentTerm), "gcc-13").has_value());
    CHECK(term.Check().Checked());
    CHECK(term.Check().Expected() == CurrentTerm);

    auto const stale = (*validator)(GrantUnder(DeposedTerm), "gcc-13");
    REQUIRE(stale.has_value());
    CHECK(stale->reason == Distributed::LeaseRefusalReason::EpochMismatch);
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
    NullLogger logger;

    auto validator = MakeWorkerLeaseValidator(cfg, ThisWorker, SocketActivation::No, LeaseClock, term, logger);
    REQUIRE(validator.has_value());

    CHECK_FALSE((*validator)(GrantUnder(CurrentTerm), "gcc-13").has_value());
    CHECK_FALSE(term.Check().Checked());
}

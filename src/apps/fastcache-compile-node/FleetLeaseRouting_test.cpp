// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <Dispatch.hpp>
#include <tests/FleetHarness.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using FastCache::Testing::Unwrap;

namespace
{
namespace Wire = CompileCacheWire;

constexpr std::string_view SchedulerA = "sched-a:6676";
constexpr std::string_view SchedulerB = "sched-b:6676";
constexpr std::string_view Worker = "worker-1:6677";
constexpr std::string_view Toolchain = "gcc-14-x86_64";
constexpr std::string_view Key = "obj-abcdef";

/// The verb byte of one logged exchange, so a test can name verbs rather than bytes.
/// @param op The verb.
/// @return Its wire byte.
[[nodiscard]] std::uint8_t Raw(Wire::Op op) noexcept
{
    return static_cast<std::uint8_t>(op);
}

/// A dispatch request for `Key` against `Toolchain`, pointed at @p scheduler.
/// @param scheduler The `--scheduler` endpoint this client was configured with.
/// @return The request.
[[nodiscard]] Cc::DispatchRequest RequestVia(std::string_view scheduler)
{
    return Cc::DispatchRequest { .schedulerEndpoint = scheduler,
                                 .fingerprint = Toolchain,
                                 .objectKey = Key,
                                 .args = {},
                                 .preprocessed = "int main() { return 0; }",
                                 .sourceName = "main.cpp",
                                 .compileDir = {},
                                 .compileDirReplacement = {} };
}

/// Take a lease for `Key` from @p scheduler the way a second client would.
///
/// Through the harness's own exchange rather than by calling `SchedulerService`
/// directly, so this second client frames its request exactly as the first one
/// does — the collision the case is about is between two *clients*, and reaching
/// past the wire to arrange it would be arranging something else.
/// @param fleet The fleet.
/// @param scheduler Who to ask.
/// @return The granted token.
[[nodiscard]] std::string LeaseFrom(Testing::FleetHarness& fleet, std::string_view scheduler)
{
    auto const outcome =
        fleet.Exchange(scheduler,
                       Wire::EncodeLease(Wire::LeaseRequest { .fingerprint = Toolchain, .key = Key, .acceptedCodecs = {} }),
                       Cc::Credential {},
                       Cc::ExchangeBudget {});
    REQUIRE(outcome.IsHit());
    auto const grant = Wire::DecodeLeaseGrant(outcome.value);
    REQUIRE(grant.has_value());
    return std::string { Wire::AsStringView(Unwrap(grant).leaseToken) };
}

} // namespace

TEST_CASE("A release goes to the scheduler that issued the lease, not to the configured one", "[node][fleet]")
{
    // **The interleaving is the whole case.** Every part of this is asserted
    // somewhere in isolation already -- `LeaseTable` resolves a token, `Gate()`
    // refuses a follower, `SchedulerLink` follows a redirect -- and none of that
    // reaches the rule, which is about a SEQUENCE across two machines: a lease
    // granted by one scheduler, leadership moving, and the release still going to
    // the machine that actually holds the lease.
    //
    // Leases are per-scheduler state. They are not replicated, so a release sent to
    // the wrong one does not merely fail to resolve anything -- it can resolve
    // SOMEBODY ELSE'S, because two schedulers number their leases independently and
    // both start at one. That is the harm, and it is why this is a safety property
    // rather than a tidiness one.
    Testing::FleetHarness fleet;
    fleet.AddScheduler(std::string { SchedulerA });
    fleet.AddScheduler(std::string { SchedulerB });

    // Each scheduler has to be leading to accept a registration -- `Gate()` runs for
    // every verb -- so the fleet is built one leadership at a time. Both end up
    // knowing the same worker, which is what an election between them looks like.
    fleet.ElectLeader(SchedulerA);
    fleet.RegisterWorker(SchedulerA, Worker, Toolchain);
    fleet.ElectLeader(SchedulerB);
    fleet.RegisterWorker(SchedulerB, Worker, Toolchain);

    // Between the grant and the release: leadership moves back to A, and a second
    // client takes A's own lease on the SAME key. A is now the leader, so this is
    // an ordinary thing for a second client to do -- and A's lease table, being
    // A's, hands out the same first token B did.
    std::string secondToken;
    fleet.OnCompile([&] {
        fleet.ElectLeader(SchedulerA);
        secondToken = LeaseFrom(fleet, SchedulerA);
    });

    // The first client, configured with A, is redirected to B and leases there.
    auto const request = RequestVia(SchedulerA);
    auto const result = Cc::Dispatch(fleet, request, Cc::DispatchBudgets {}, Cc::Credential {}, {});
    CHECK(result.status == Cc::DispatchStatus::Declined); // the harness worker refuses; the release still happens

    // The two clients really did collide on a token, or the case below would pass
    // for the wrong reason -- a release to the wrong scheduler is only *harmful*
    // when the number means something there too.
    CHECK_FALSE(secondToken.empty());

    // The release went to the issuer.
    auto const& calls = fleet.Calls();
    REQUIRE_FALSE(calls.empty());
    auto const& last = calls.back();
    CHECK(last.opRaw == Raw(Wire::Op::Release));
    CHECK(last.endpoint == SchedulerB);

    // And the consequence, which is the part worth having: the second client's
    // lease is INTACT. Sent to the configured endpoint instead, this release would
    // have matched -- both tables number from one and both leases name the same key
    // -- and freed a key another client is still building, on the machine that would
    // then hand it to a third.
    CHECK(fleet.IsInFlight(SchedulerA, Key));

    // **And the release SETTLED, which is #371.** The issuer is a follower by now,
    // and this used to be refused `NotLeader` -- so the key stayed pinned on the one
    // machine that could free it until it expired, while the client had done exactly
    // the right thing. A release is not a scheduling decision: it resolves a lease
    // this node minted, in a table nobody else holds a copy of.
    //
    // These three lines were written the other way round when the harness first found
    // this, asserting the refusal and pointing at #371. They flip together, which is
    // the point of having pinned the wrong behaviour explicitly rather than leaving it
    // as a silent surprise.
    CHECK(last.kind == Cc::CacheOutcomeKind::Hit);
    CHECK_FALSE(fleet.IsInFlight(SchedulerB, Key));
}

TEST_CASE("A demoted scheduler settles its own lease and still refuses one it never issued", "[node][fleet]")
{
    // The other half of #371's acceptance, and the half that turns a fix into a hole
    // if it is missing. Letting a release through after demotion must not mean letting
    // ANY release through: a token this node never issued resolves nothing, and saying
    // so is the only place "this job outlived its lease" can be observed.
    Testing::FleetHarness fleet;
    fleet.AddScheduler(std::string { SchedulerA });
    fleet.AddScheduler(std::string { SchedulerB });

    fleet.ElectLeader(SchedulerB);
    fleet.RegisterWorker(SchedulerB, Worker, Toolchain);
    auto const token = LeaseFrom(fleet, SchedulerB);
    REQUIRE(fleet.IsInFlight(SchedulerB, Key));

    // B is demoted with the lease still outstanding.
    fleet.ElectLeader(SchedulerA);

    // A token B never minted is refused, by name, and does not disturb the real one.
    auto const bogus = fleet.Exchange(SchedulerB,
                                      Wire::EncodeRelease(Wire::ReleaseRequest { .leaseToken = "not-a-token", .key = Key }),
                                      Cc::Credential {},
                                      Cc::ExchangeBudget {});
    CHECK(bogus.kind == Cc::CacheOutcomeKind::Rejected);
    CHECK(bogus.code == Wire::ErrorCode::UnknownLease);
    CHECK(fleet.IsInFlight(SchedulerB, Key));

    // The right token against the WRONG key is refused too -- `LeaseTable` matches on
    // both, so a release cannot free a key it does not name.
    auto const wrongKey =
        fleet.Exchange(SchedulerB,
                       Wire::EncodeRelease(Wire::ReleaseRequest { .leaseToken = token, .key = "obj-somebody-else" }),
                       Cc::Credential {},
                       Cc::ExchangeBudget {});
    CHECK(wrongKey.kind == Cc::CacheOutcomeKind::Rejected);
    CHECK(wrongKey.code == Wire::ErrorCode::UnknownLease);
    CHECK(fleet.IsInFlight(SchedulerB, Key));

    // And the genuine one settles, from a node that is no longer the leader.
    auto const real = fleet.Exchange(SchedulerB,
                                     Wire::EncodeRelease(Wire::ReleaseRequest { .leaseToken = token, .key = Key }),
                                     Cc::Credential {},
                                     Cc::ExchangeBudget {});
    CHECK(real.kind == Cc::CacheOutcomeKind::Hit);
    CHECK_FALSE(fleet.IsInFlight(SchedulerB, Key));

    // Releasing it a second time is refused rather than silently accepted: the entry
    // is gone, and a client told nothing has nothing to report.
    auto const again = fleet.Exchange(SchedulerB,
                                      Wire::EncodeRelease(Wire::ReleaseRequest { .leaseToken = token, .key = Key }),
                                      Cc::Credential {},
                                      Cc::ExchangeBudget {});
    CHECK(again.kind == Cc::CacheOutcomeKind::Rejected);
    CHECK(again.code == Wire::ErrorCode::UnknownLease);
}

TEST_CASE("A lease taken from the configured leader is released back to it", "[node][fleet]")
{
    // The straight case, so the one above cannot pass by accident: with no redirect
    // and no election, the issuer and the configured endpoint are the same machine,
    // and a client that always released to whoever granted would look identical to
    // one that always released to its configured address. Only the redirect tells
    // them apart -- which is the point, and the reason both cases are here.
    Testing::FleetHarness fleet;
    fleet.AddScheduler(std::string { SchedulerA });
    fleet.ElectLeader(SchedulerA);
    fleet.RegisterWorker(SchedulerA, Worker, Toolchain);

    auto const request = RequestVia(SchedulerA);
    (void) Cc::Dispatch(fleet, request, Cc::DispatchBudgets {}, Cc::Credential {}, {});

    auto const& calls = fleet.Calls();
    REQUIRE(calls.size() == 3); // LEASE, COMPILE, RELEASE -- no redirect hop
    CHECK(calls.front().endpoint == SchedulerA);
    CHECK(calls.back().opRaw == Raw(Wire::Op::Release));
    CHECK(calls.back().endpoint == SchedulerA);
    CHECK_FALSE(fleet.IsInFlight(SchedulerA, Key));
}

TEST_CASE("A lease outliving its holder stops suppressing its key once time moves", "[node][fleet]")
{
    // Expiry is the third of a lease's three transitions and the only one no client
    // performs, so it is the one an in-process fleet can assert without a process
    // that has to actually die. Time moves in `Step` and nowhere else.
    Testing::FleetHarness fleet;
    fleet.AddScheduler(std::string { SchedulerA });
    fleet.ElectLeader(SchedulerA);
    fleet.RegisterWorker(SchedulerA, Worker, Toolchain);

    auto const token = LeaseFrom(fleet, SchedulerA);
    CHECK_FALSE(token.empty());
    CHECK(fleet.IsInFlight(SchedulerA, Key));

    // Nothing releases it -- this is the client that died.
    fleet.Step(Distributed::LeaseTable::DefaultLeaseTimeout + std::chrono::seconds { 1 });
    CHECK_FALSE(fleet.IsInFlight(SchedulerA, Key));
}

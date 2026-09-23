// SPDX-License-Identifier: Apache-2.0
//
// #1471, clauses 2 and 3: a client forget committed on the leader must be REFUSED at a second
// machine and REPORTED by that machine's `--node-status`, and a machine that has not applied the
// entry must report differently.
//
// #1309 proved each layer apart -- the oracle, the publish, the apply, the refusal -- and did not
// prove them composed across a pair. So every case here drives all three at once:
//
//   * the real `SchedulerService` and `SchedulerProtocol`, through `FleetHarness`, decide whether
//     the caller is served,
//   * a real `NodeMembership` per machine holds what that machine has applied,
//   * a real `ConfiguredNodeStatus` per machine renders what an operator would read.
//
// **The caller host is not loopback, and it cannot be.** Loopback is never forgotten -- a node
// always admits its own machine, by design -- so a forget published against `127.0.0.1` applies
// to nobody and every assertion below would pass over a fleet that had refused nothing. That is
// why `FleetHarness::SetCallerHost` exists, and it is the first thing each case sets.
//
// **The refusal is asserted by CODE, never by "the exchange did not succeed".** A dozen
// arrangements decline a lease -- no worker, no capacity, a follower with no leader -- so a case
// checking only that it failed would pass under every one of them and prove nothing about a
// forget. `Wire::ErrorCode::NotAMember` is the fact under test.
#include "NodeConfig.hpp"
#include "NodeMembership.hpp"
#include "NodeStatusResponder.hpp"

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <Dispatch.hpp>
#include <tests/FleetHarness.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;
namespace Wire = CompileCacheWire;
using FastCache::Testing::Unwrap;

namespace
{

/// The client this fleet forgets. NOT loopback -- see the file header.
constexpr std::string_view ForgottenClient = "10.0.0.7";

/// Where the machine that has APPLIED the entry answers.
///
/// There is no separate endpoint for "the leader that committed it": each machine here answers
/// as the elected scheduler of its own fleet, for the reason the cases state -- a follower
/// refuses on leadership before membership is ever consulted. What distinguishes the machines is
/// what each has applied, which is the subject.
constexpr std::string_view Applied = "sched-b:6676";

/// A third machine, deliberately left BEHIND on the log.
constexpr std::string_view Behind = "sched-c:6676";

/// The toolchain every worker here serves.
constexpr std::string_view Toolchain = "tc-1";

/// The object every lease here is for.
constexpr std::string_view Key = "obj-1";

/// One machine: its applied membership and the status an operator would read from it.
///
/// A struct rather than three locals per case because the two must be held together: the status
/// binds `NodeConfig const&`, so a configuration that dies first is a dangling read -- the same
/// finding `NodeStatusResponder_test`'s own fixture records, and the reason there is no way to
/// spell the temporary here either.
struct Machine
{
    /// @param clock Where uptime comes from; must outlive this.
    explicit Machine(IClock const& clock):
        // **`--fleet-member` NAMES the client, and without it this whole file tests nothing.**
        // A default `NodeConfig` lists no members, and an empty list refuses everybody but
        // loopback -- so the client would be refused as an OUTSIDER whether or not anything was
        // forgotten, and both refusals are `NotAMember` on the wire. Measured: the control
        // section caught it as `19 != 19`.
        //
        // This is also the arrangement #1471 is actually about -- "the host an operator has just
        // forgotten in the cluster is exactly the one still named by `--fleet-member` on a node
        // nobody has reconfigured". The forget has to OUTRANK a live listing, and a node that
        // never admitted the host cannot demonstrate that.
        cfg { [] {
            auto c = NodeConfig {};
            c.fleetMembers = { std::string { ForgottenClient } };
            return c;
        }() },
        status {
            cfg, clock, clock.now(), "1.2.3", "node-x", NodeComponents {}, NodeRuntimeSources { .membership = &membership }
        }
    {
    }

    NodeConfig cfg;
    NullLogger logger;
    NodeMembership membership { cfg, logger };
    ConfiguredNodeStatus status;

    /// Apply a committed forget, as consensus would.
    /// @param hosts Who the cluster has forgotten.
    void Apply(std::vector<std::string> hosts)
    {
        auto state = Cluster::ClusterState {};
        state.forgotten = std::move(hosts);
        membership.PublishCluster(state);
    }

    /// What `--node-status` would report for the applied-tombstone count.
    /// @return The count, or nothing when this machine reports none.
    [[nodiscard]] std::optional<std::uint32_t> Reported() const
    {
        return status.Describe().runtime.forgottenClients;
    }
};

/// Ask @p scheduler for a lease, as a client does.
/// @param fleet The fleet.
/// @param scheduler Who to ask.
/// @return The exchange's outcome, carrying the refusal CODE when it refused.
[[nodiscard]] Cc::CacheOutcome AskForLease(Testing::FleetHarness& fleet, std::string_view scheduler)
{
    return fleet.Exchange(
        scheduler,
        Wire::EncodeLease(Wire::LeaseRequest { .fingerprint = Toolchain, .key = Key, .acceptedCodecs = {} }),
        Cc::Credential {},
        Cc::ExchangeBudget {});
}

} // namespace

TEST_CASE("a forget committed on the leader is refused at a second machine AND reported by it",
          "[node][fleet][forget][membership]")
{
    // #1471's second acceptance clause, composed rather than layered.
    // **The machine under test is the ELECTED scheduler, and that is a constraint of the
    // harness rather than a modelling choice.** `Gate()` runs leadership and membership
    // together, and leadership is refused FIRST: a follower answers `NotLeader` before
    // membership is ever consulted, so asking one could never produce `NotAMember` and the case
    // would assert the wrong refusal. It also refuses `Register`, so a worker cannot even be
    // arranged there. So each machine is driven as the answering leader of its own fleet, and
    // what distinguishes the two is what each has APPLIED -- which is the subject anyway.
    ManualClock clock;
    Testing::FleetHarness fleet;
    fleet.AddScheduler(std::string { Applied });
    fleet.ElectLeader(Applied);
    fleet.RegisterWorker(Applied, "worker-b:6677", Toolchain);

    // Not loopback. A forget against `127.0.0.1` applies to nobody, so this line is what makes
    // every assertion below able to fail at all.
    fleet.SetCallerHost(std::string { ForgottenClient });

    Machine leader { clock };
    Machine applied { clock };
    fleet.SetMembershipAt(Applied, &applied.membership.Oracle());

    SECTION("before the forget, the client is NOT refused for membership and neither machine reports one")
    {
        // THE CONTROL, and it is not decoration: without it a fleet that refused this client for
        // some other reason -- an unregistered toolchain, a follower with no leader -- would make
        // the refusal below pass while proving nothing about the forget.
        auto const outcome = AskForLease(fleet, Applied);
        CHECK(outcome.code != Wire::ErrorCode::NotAMember);

        REQUIRE(leader.Reported().has_value());
        CHECK(Unwrap(leader.Reported()) == 0);
        REQUIRE(applied.Reported().has_value());
        CHECK(Unwrap(applied.Reported()) == 0);
    }

    SECTION("the entry commits on the leader and is applied at the second machine")
    {
        leader.Apply({ std::string { ForgottenClient } });
        applied.Apply({ std::string { ForgottenClient } });

        // REFUSED, and by MEMBERSHIP -- the code, not merely a failure.
        auto const outcome = AskForLease(fleet, Applied);
        CHECK(outcome.code == Wire::ErrorCode::NotAMember);

        // AND REPORTED. Both halves, because either alone is green under half the defect: a
        // refusal nothing reports leaves an operator with no way to confirm the forget landed,
        // and a report with no refusal is a node that counts an entry it does not enforce.
        REQUIRE(applied.Reported().has_value());
        CHECK(Unwrap(applied.Reported()) == 1);
    }
}

TEST_CASE("a machine that has NOT applied the forget reports differently, and still serves the client",
          "[node][fleet][forget][membership]")
{
    // #1471's third acceptance clause: *prove the reporting can fail.* If a machine that is
    // behind renders the same as one that has applied the entry, the verb answers nothing -- so
    // the two readings are compared against EACH OTHER rather than each against a constant.
    // The behind machine answers as its own fleet's leader, for the reason the case above
    // states: a follower refuses on leadership before membership is reached, which would make
    // "it still serves the client" untestable.
    ManualClock clock;
    Testing::FleetHarness fleet;
    fleet.AddScheduler(std::string { Behind });
    fleet.ElectLeader(Behind);
    fleet.RegisterWorker(Behind, "worker-c:6677", Toolchain);
    fleet.SetCallerHost(std::string { ForgottenClient });

    Machine leader { clock };
    Machine behind { clock };
    fleet.SetMembershipAt(Behind, &behind.membership.Oracle());

    // Committed on the leader ONLY. This is a fleet mid-propagation, which is the state an
    // operator meets between issuing `--cluster-forget-client` and it taking effect everywhere.
    leader.Apply({ std::string { ForgottenClient } });

    SECTION("the two machines report DIFFERENT counts, which is what makes the verb worth reading")
    {
        REQUIRE(leader.Reported().has_value());
        REQUIRE(behind.Reported().has_value());
        // Compared against each other first: what the clause asks is that they DIFFER, and two
        // constant assertions would still pass if both were wrong in the same direction.
        CHECK(Unwrap(leader.Reported()) != Unwrap(behind.Reported()));
        CHECK(Unwrap(leader.Reported()) == 1);
        CHECK(Unwrap(behind.Reported()) == 0);
    }

    SECTION("and the behind machine still SERVES the client, which is why the difference matters")
    {
        // The reason the report is worth having at all. This machine is not refusing yet, so an
        // operator who believed the forget was in force fleet-wide would be wrong -- and the
        // count is the only thing that tells them which machine is which.
        //
        // Both ends in one section deliberately: a differing count whose machines behaved
        // identically would be a number with nothing behind it.
        // The behind machine does not refuse on membership...
        CHECK(AskForLease(fleet, Behind).code != Wire::ErrorCode::NotAMember);

        // ...and the SAME client against a machine that HAS applied the entry does. Its own
        // fleet, because each machine has to answer as a leader; what differs between the two
        // exchanges is only which machine's oracle decided, which is the comparison the clause
        // asks for.
        Testing::FleetHarness enforcing;
        enforcing.AddScheduler(std::string { Applied });
        enforcing.ElectLeader(Applied);
        enforcing.RegisterWorker(Applied, "worker-b:6677", Toolchain);
        enforcing.SetCallerHost(std::string { ForgottenClient });
        enforcing.SetMembershipAt(Applied, &leader.membership.Oracle());
        CHECK(AskForLease(enforcing, Applied).code == Wire::ErrorCode::NotAMember);
    }
}

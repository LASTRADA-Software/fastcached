// SPDX-License-Identifier: Apache-2.0
//
// #1471, clauses 2 and 3, restated for a forget that is a key revocation: a forget committed on
// the leader must be REFUSED at a second machine, attributed there to the revoked key, and a
// machine that has not applied the entry must answer differently.
//
// #1309 proved each layer apart -- the oracle, the publish, the apply, the refusal -- and did not
// prove them composed across a pair. So every case here drives them at once:
//
//   * the real `SchedulerService` and `SchedulerProtocol`, through `FleetHarness`, decide whether
//     the caller is served,
//   * a real `NodeMembership` per machine holds what that machine has applied, and folds the
//     identity the caller's connection proved exactly as production's endpoint does.
//
// **A machine is forgotten by its key, and an address is not an identity.** The caller here
// proves an identity key (`FleetHarness::SetCallerIdentity`); the forget revokes that key; and the
// refusal is asserted from an address that is NOT the one the machine was admitted at, beside a
// control from the same address under a key nobody revoked. A case that forgot a host would pass
// over a build that still refused by address.
//
// **The refusal is asserted by CODE, never by "the exchange did not succeed".** A dozen
// arrangements decline a lease -- no worker, no capacity, a follower with no leader -- so a case
// checking only that it failed would pass under every one of them and prove nothing about a
// forget. `Wire::ErrorCode::NotAMember` is the fact under test.
#include "NodeConfig.hpp"
#include "NodeMembership.hpp"

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/SchedulerService.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>
#include <FastCache/Protocol/ProvenIdentity.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <string_view>

#include <Dispatch.hpp>
#include <tests/FleetHarness.hpp>
#include <tests/RaftPeerKeyFakes.hpp>

using namespace FastCache;
using namespace FastCache::Node;
namespace Wire = CompileCacheWire;

namespace
{

/// The machine this fleet forgets, by the id it proves.
constexpr std::string_view ForgottenMachine = "laptop-7";

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

/// The identity @p id proves: its test key.
/// @param id The machine.
/// @return What its connection proved.
[[nodiscard]] ProvenIdentity Proving(std::string_view id)
{
    auto const machine = std::string { id };
    return ProvenIdentity { .id = machine, .key = Testing::TestKeyPair(machine).PublicKey() };
}

/// One machine: its applied membership.
///
/// A struct because the two must be held together: `NodeMembership` binds `NodeConfig const&`,
/// so a configuration that dies first is a dangling read.
struct Machine
{
    Machine():
        // **`--fleet-open` ADMITS the caller by its address, and without it this whole file
        // tests nothing.** A default `NodeConfig` admits nobody but loopback and the roster's
        // keys -- so the caller would be refused as an OUTSIDER whether or not anything was
        // forgotten, and both refusals are `NotAMember` on the wire.
        //
        // The forget has to OUTRANK a live admission on a node nobody has reconfigured, and a
        // node that never admitted the caller cannot demonstrate that. The open flag is that
        // admission, and a revoked key outranks it.
        cfg { [] {
            auto c = NodeConfig {};
            c.fleetOpen = true;
            return c;
        }() }
    {
    }

    NodeConfig cfg;
    NullLogger logger;
    NodeMembership membership { cfg, logger };

    /// Apply a committed forget of @p id, as consensus would: its learner record gone and its
    /// key revoked -- `Apply` over the admission and the forget, so the state is one the cluster
    /// could hold.
    /// @param id The machine forgotten.
    void ApplyForget(std::string_view id)
    {
        auto state = Cluster::ClusterState {};
        auto const machine = std::string { id };
        Cluster::Apply(state,
                       Cluster::Command { .kind = Cluster::CommandKind::AddLearner,
                                          .key = machine,
                                          .value = {},
                                          .schedulerEndpoint = {},
                                          .publicKey = Testing::TestKeyPair(machine).PublicKey() });
        Cluster::Apply(state,
                       Cluster::Command { .kind = Cluster::CommandKind::Forget,
                                          .key = machine,
                                          .value = {},
                                          .schedulerEndpoint = {},
                                          .publicKey = std::nullopt });
        membership.PublishCluster(state);
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

TEST_CASE("a forget committed on the leader is refused at a second machine, by the key it revoked",
          "[node][fleet][forget][membership]")
{
    // #1471's second acceptance clause, composed rather than layered.
    // **The machine under test is the ELECTED scheduler, and that is a constraint of the
    // harness rather than a modelling choice.** `Gate()` runs leadership and membership
    // together, and leadership is refused FIRST: a follower answers `NotLeader` before
    // membership is ever consulted, so asking one could never produce `NotAMember`. So the
    // machine is driven as the answering leader of its own fleet, and what distinguishes it is
    // what it has APPLIED -- which is the subject anyway.
    Testing::FleetHarness fleet;
    fleet.AddScheduler(std::string { Applied });
    fleet.ElectLeader(Applied);
    fleet.RegisterWorker(Applied, "worker-b:6677", Toolchain);

    Machine applied;
    fleet.SetMembershipAt(Applied, &applied.membership.Oracle());
    fleet.SetCallerHost("10.0.0.7");
    fleet.SetCallerIdentity(Proving(ForgottenMachine));

    SECTION("before the forget, the machine is NOT refused for membership")
    {
        // THE CONTROL, and it is not decoration: without it a fleet that refused this caller for
        // some other reason -- an unregistered toolchain, a follower with no leader -- would make
        // the refusal below pass while proving nothing about the forget.
        CHECK(AskForLease(fleet, Applied).code != Wire::ErrorCode::NotAMember);
    }

    SECTION("once applied, the machine is refused from any address, and its key is what decided")
    {
        applied.ApplyForget(ForgottenMachine);

        // REFUSED, and by MEMBERSHIP -- the code, not merely a failure -- and from an address
        // the machine never dialled from before, since an address is not an identity.
        CHECK(AskForLease(fleet, Applied).code == Wire::ErrorCode::NotAMember);
        fleet.SetCallerHost("10.9.9.9");
        CHECK(AskForLease(fleet, Applied).code == Wire::ErrorCode::NotAMember);

        // Attributed to the revocation, which is what an operator asking `explain-admission`
        // reads: the removed machine, not a stranger.
        auto const decision = Distributed::ExplainConnection(
            applied.membership.Oracle(), ConnectionFacts { .host = "10.9.9.9", .proven = Proving(ForgottenMachine) });
        CHECK(decision.verdict == Distributed::Membership::Forgotten);
        CHECK(decision.decidedBy.Has(Distributed::MembershipParticipant::KeyTombstone));

        // And the same address under a key nobody revoked is served: what was refused is the
        // KEY, never where it dialled from.
        fleet.SetCallerIdentity(Proving("laptop-8"));
        CHECK(AskForLease(fleet, Applied).code != Wire::ErrorCode::NotAMember);
    }
}

TEST_CASE("a machine that has NOT applied the forget still admits the key, and one that has refuses it",
          "[node][fleet][forget][membership]")
{
    // #1471's third acceptance clause: *prove the difference can be seen.* A fleet
    // mid-propagation, which is the state an operator meets between issuing `--cluster-forget`
    // and it taking effect everywhere -- the two machines answer the SAME caller, proving the SAME
    // key, differently. Each answers as its own fleet's leader, for the reason the case above
    // states: a follower refuses on leadership before membership is reached.
    Machine enforcing;
    Machine behind;
    enforcing.ApplyForget(ForgottenMachine);

    Testing::FleetHarness behindFleet;
    behindFleet.AddScheduler(std::string { Behind });
    behindFleet.ElectLeader(Behind);
    behindFleet.RegisterWorker(Behind, "worker-c:6677", Toolchain);
    behindFleet.SetMembershipAt(Behind, &behind.membership.Oracle());
    behindFleet.SetCallerHost("10.0.0.7");
    behindFleet.SetCallerIdentity(Proving(ForgottenMachine));

    Testing::FleetHarness enforcingFleet;
    enforcingFleet.AddScheduler(std::string { Applied });
    enforcingFleet.ElectLeader(Applied);
    enforcingFleet.RegisterWorker(Applied, "worker-b:6677", Toolchain);
    enforcingFleet.SetMembershipAt(Applied, &enforcing.membership.Oracle());
    enforcingFleet.SetCallerHost("10.0.0.7");
    enforcingFleet.SetCallerIdentity(Proving(ForgottenMachine));

    // Both ends in one case deliberately: a machine that refused here while the other served
    // would be the whole fleet agreeing, and one that served while the other refused is the
    // difference the clause asks to see. What differs between the two exchanges is only which
    // machine's applied state decided.
    CHECK(AskForLease(behindFleet, Behind).code != Wire::ErrorCode::NotAMember);
    CHECK(AskForLease(enforcingFleet, Applied).code == Wire::ErrorCode::NotAMember);
}

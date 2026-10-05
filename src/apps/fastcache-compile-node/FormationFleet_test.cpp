// SPDX-License-Identifier: Apache-2.0
#include "ForeignFleetWatch.hpp"
#include "FormationController.hpp"
#include "NodeAnnounce.hpp"
#include "NodeConditions.hpp"
#include "SharedCacheResponder.hpp"

#include <FastCache/Cluster/NodeMode.hpp>
#include <FastCache/Core/WireFrame.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <core/platform/Clock.hpp>
#include <tests/FormationHarness.hpp>
#include <tests/NodeConditionFakes.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using namespace std::chrono_literals;

namespace Wire = CompileCacheWire;
using Lan = Testing::FormationHarness;
using Testing::Unwrap;

// A whole office in one process (`Testing::FormationHarness`): every machine runs the real discovery,
// watch, formation controller, enrollment and fleet-summary surfaces, and production's dialled
// channel and probe between them. These are spec section 8's multi-node cases; consensus itself is
// `RaftClusterHarness`'s, and a per-cluster applied state stands in for it here.
//
// Every span is derived from production's cadence (`Lan::MeetWithin`, `Lan::TickEvery`), so a case
// that passes here passes at the intervals a machine actually runs at.

namespace
{
/// How long a joiner takes to hear an approval and adopt it: a poll, and the beat that adopts it.
constexpr std::chrono::seconds Adopted = 2 * Lan::TickEvery;
} // namespace

TEST_CASE("Two solitary machines meet and exactly one yields", "[node][formation][fleet]")
{
    core::platform::ManualClock clock;
    core::platform::ManualWallClock wall;
    Lan office { clock, wall };
    office.AddMachine("n-a", 100);
    office.AddMachine("n-b", 100); // the same second: the lower cluster id, `c-n-a`, is kept
    office.RunFor(Lan::MeetWithin);
    INFO("n-a said:\n" << office.LogOf("n-a") << "n-b said:\n" << office.LogOf("n-b"));
    CHECK(office.ModeOf("n-b") == Cluster::NodeMode::Pending);
    CHECK(office.ModeOf("n-a") == Cluster::NodeMode::Solitary);
}

TEST_CASE("An established fleet never yields and both fleets raise foreign-fleet-visible", "[node][formation][fleet]")
{
    core::platform::ManualClock clock;
    core::platform::ManualWallClock wall;
    Lan lan { clock, wall };
    for (auto const* const id: { "n-office", "n-office-2", "n-lab", "n-lab-2" })
        lan.AddMachine(id, 100);

    // The office forms while the lab is unplugged -- `c-n-office` is the lower id, so n-office-2 asks...
    lan.SetOnLan("n-lab", false);
    lan.SetOnLan("n-lab-2", false);
    lan.RunFor(Lan::MeetWithin);
    lan.Approve("n-office", "n-office-2");
    lan.RunFor(Adopted);
    REQUIRE(lan.ModeOf("n-office-2") == Cluster::NodeMode::Learner);

    // ...and the lab while the office is.
    lan.SetOnLan("n-lab", true);
    lan.SetOnLan("n-lab-2", true);
    lan.SetOnLan("n-office", false);
    lan.SetOnLan("n-office-2", false);
    lan.RunFor(Lan::MeetWithin);
    lan.Approve("n-lab", "n-lab-2");
    lan.RunFor(Adopted);
    REQUIRE(lan.ModeOf("n-lab-2") == Cluster::NodeMode::Learner);

    lan.SetOnLan("n-office", true);
    lan.SetOnLan("n-office-2", true);
    lan.RunFor(2 * Lan::MeetWithin);

    CHECK(lan.ClusterOf("n-office") == lan.ClusterOf("n-office-2"));
    CHECK(lan.ClusterOf("n-lab") == lan.ClusterOf("n-lab-2"));
    CHECK(lan.ClusterOf("n-office") != lan.ClusterOf("n-lab"));
    CHECK(lan.ConditionsOf("n-office").StateOf(NodeCondition::ForeignFleetVisible) == Wire::ConditionState::Raised);
    CHECK(lan.ConditionsOf("n-lab").StateOf(NodeCondition::ForeignFleetVisible) == Wire::ConditionState::Raised);
}

TEST_CASE("A healthy single-fleet office reads both foreign-fleet rows clear", "[node][formation][fleet][conditions]")
{
    // Nobody else on the segment, and both members heard each other: "no other fleet" is a FINDING,
    // and both rows say clear -- never not-evaluated with "no discovery reply heard", which a watch
    // told only of OTHER fleets would say here forever.
    core::platform::ManualClock clock;
    core::platform::ManualWallClock wall;
    Lan lan { clock, wall };
    lan.AddMachine("n-office", 100);
    lan.AddMachine("n-laptop", 500);
    lan.RunFor(Lan::MeetWithin);
    lan.Approve("n-office", "n-laptop");
    lan.RunFor(Adopted);
    REQUIRE(lan.ModeOf("n-laptop") == Cluster::NodeMode::Learner);
    REQUIRE(lan.ClusterOf("n-laptop") == lan.ClusterOf("n-office"));

    lan.RunFor(Lan::MeetWithin);
    for (auto const* const id: { "n-office", "n-laptop" })
        for (auto const condition: ForeignFleetWatch::WatchedConditions)
        {
            INFO(id << ": " << Testing::DetailOf(lan.ConditionsOf(id), condition));
            CHECK(lan.ConditionsOf(id).StateOf(condition) == Wire::ConditionState::Clear);
        }
}

TEST_CASE("A request the node port's gate refuses never reaches the surface behind it", "[node][formation][fleet][gate]")
{
    // The harness's node port asks every header what the frame endpoint asks it (`DecideHeaderRefusal`):
    // a control frame past the verb's ceiling is refused PayloadTooLarge before a responder sees it --
    // where the responder itself would have answered with a refusal of its own.
    core::platform::ManualClock clock;
    core::platform::ManualWallClock wall;
    Lan lan { clock, wall };
    lan.AddMachine("n-office", 100);
    auto const oversized =
        Wire::EncodeEnrollControl(Wire::EnrollControlVerb::Reject, std::string(std::size_t { 1 } << 20, 'x'));
    auto const reply = lan.AskAsOperator("n-office", oversized);
    auto const header = Wire::DecodeReplyHeader(reply);
    REQUIRE(header.has_value());
    REQUIRE(Unwrap(header).status == Wire::Status::Error);
    auto const refusal = Wire::DecodeErrorPayload(
        std::span<std::byte const> { reply }.subspan(Wire::ReplyHeaderSize, Unwrap(header).payloadLength));
    REQUIRE(refusal.has_value());
    CHECK(Unwrap(refusal).first == Wire::ErrorCode::PayloadTooLarge);
}

TEST_CASE("The node port merges every owner a built node merges, and its ceiling is theirs",
          "[node][formation][fleet][gate]")
{
    // The harness composes its surface through `ComposeSurfaceComponents`, as the node does, and its
    // gate reads that surface's own `MaxRequestBytes()`: so the operator owners every built node merges
    // are there to answer, and the ceiling is theirs rather than a number the harness supplies.
    core::platform::ManualClock clock;
    core::platform::ManualWallClock wall;
    Lan lan { clock, wall };
    lan.AddMachine("n-office", 100);

    auto status = std::vector<std::byte>(Wire::RequestHeaderSize);
    WireFrame::PutHeader(status, Wire::Magic, Wire::CurrentVersion, static_cast<std::uint8_t>(Wire::Op::NodeStatus), 0);
    auto const reply = lan.AskAsOperator("n-office", status);
    auto const header = Wire::DecodeReplyHeader(reply);
    REQUIRE(header.has_value());
    CHECK(Unwrap(header).status == Wire::Status::Ok);
    // The fleet's shared cache, which every node merges dormant, declares a store's size -- the largest
    // any owner this body merges declares (the operator owners' is `MaxControlPayload`, the
    // fleet-summary owner's far less): the fold the frame endpoint reads. The verbs' own ceilings keep
    // it off every control verb, which the case above asserts.
    CHECK(lan.SurfaceCeilingOf("n-office") == SharedCacheResponder::SharedCacheMaxRequestBytes);
}

TEST_CASE("A pending machine keeps serving its own compiles until it is approved", "[node][formation][fleet]")
{
    core::platform::ManualClock clock;
    core::platform::ManualWallClock wall;
    Lan lan { clock, wall };
    lan.AddMachine("n-office", 100);
    lan.AddMachine("n-laptop", 500);
    lan.RunFor(Lan::MeetWithin);
    REQUIRE(lan.ModeOf("n-laptop") == Cluster::NodeMode::Pending);
    lan.RunFor(5min);
    INFO("the laptop said:\n" << lan.LogOf("n-laptop") << "the office said:\n" << lan.LogOf("n-office"));
    CHECK(lan.ModeOf("n-laptop") == Cluster::NodeMode::Pending);
    // Leases its own scheduler GRANTED while the record said pending: served work, not a mode reading.
    auto const beforeRestart = lan.ServedWhilePending("n-laptop");
    CHECK(beforeRestart > 0);

    // Yielding moves no surface, so the body above was built while solitary. A restart builds one from
    // the PENDING record, which is where production's `ServesScheduler` is asked of that mode.
    lan.Restart("n-laptop");
    REQUIRE(lan.ModeOf("n-laptop") == Cluster::NodeMode::Pending);
    lan.RunFor(Lan::TickEvery);
    CHECK(lan.ServedWhilePending("n-laptop") > beforeRestart);

    lan.Approve("n-office", "n-laptop");
    lan.RunFor(Adopted);
    CHECK(lan.ModeOf("n-laptop") == Cluster::NodeMode::Learner);
    CHECK(lan.ClusterOf("n-laptop") == lan.ClusterOf("n-office"));
    CHECK(lan.ModeOf("n-office") == Cluster::NodeMode::Voter);
}

TEST_CASE("An armed window admits the laptop with nobody at the keyboard", "[node][formation][fleet][auto-approve]")
{
    core::platform::ManualClock clock;
    core::platform::ManualWallClock wall;
    Lan lan { clock, wall };
    lan.AddMachine("n-office", 100);
    lan.ArmAutoApprove("n-office", 15min);
    lan.AddMachine("n-laptop", 500);
    lan.RunFor(Lan::MeetWithin + Adopted);
    CHECK(lan.ModeOf("n-laptop") == Cluster::NodeMode::Learner);
    CHECK(lan.ClusterOf("n-laptop") == lan.ClusterOf("n-office"));
}

TEST_CASE("A machine on another subnet asks the fleet its typed seed names, and only through it",
          "[node][formation][fleet][seed]")
{
    core::platform::ManualClock clock;
    core::platform::ManualWallClock wall;
    Lan lan { clock, wall };
    lan.AddMachine("n-office", 100);
    lan.AddMachine("n-laptop", 500);
    lan.AddMachine("n-remote", 500);
    // Both off the office's segment: no beacon crosses. Only the laptop was handed a seed -- the remote
    // machine is the control that nothing but the seed carried the laptop there.
    lan.SetOnOtherSubnet("n-laptop");
    lan.SetOnOtherSubnet("n-remote");
    lan.Seed("n-laptop", "n-office");
    lan.RunFor(Lan::MeetWithin);
    INFO("the laptop said:\n" << lan.LogOf("n-laptop"));
    CHECK(lan.ModeOf("n-remote") == Cluster::NodeMode::Solitary);
    REQUIRE(lan.ModeOf("n-laptop") == Cluster::NodeMode::Pending);

    lan.Approve("n-office", "n-laptop");
    lan.RunFor(Adopted);
    CHECK(lan.ModeOf("n-laptop") == Cluster::NodeMode::Learner);
    CHECK(lan.ClusterOf("n-laptop") == lan.ClusterOf("n-office"));
}

TEST_CASE("Two solitary machines that meet only through each other's seed: the same one yields whichever starts first",
          "[node][formation][fleet][seed]")
{
    // Across a VPN no beacon crosses, so the seeds are the only route two clusters meet by. Each asks
    // the other's typed seed on its first beat and decides over the answer on the next
    // (`TickSolitary`): the solitary tie-break, never a mode that happens to flip between the two.
    using Order = std::array<char const*, 2>;
    for (auto const& order: { Order { "n-a", "n-b" }, Order { "n-b", "n-a" } })
    {
        INFO("started " << order[0] << " first");
        core::platform::ManualClock clock;
        core::platform::ManualWallClock wall;
        Lan lan { clock, wall };
        for (auto const* const id: order)
        {
            lan.AddMachine(id, 100); // the same second: the lower cluster id, `c-n-a`, is kept
            lan.SetOnOtherSubnet(id);
        }
        lan.Seed("n-a", "n-b");
        lan.Seed("n-b", "n-a");
        lan.RunFor(Lan::MeetWithin);
        INFO("n-a said:\n" << lan.LogOf("n-a") << "n-b said:\n" << lan.LogOf("n-b"));
        CHECK(lan.ModeOf("n-b") == Cluster::NodeMode::Pending);
        CHECK(lan.ModeOf("n-a") == Cluster::NodeMode::Solitary);

        // And it stays decided: the kept one's later probes see a pending machine, never a rival.
        lan.RunFor(2 * std::chrono::duration_cast<std::chrono::seconds>(SeedProbeInterval));
        CHECK(lan.ModeOf("n-b") == Cluster::NodeMode::Pending);
        CHECK(lan.ModeOf("n-a") == Cluster::NodeMode::Solitary);
    }
}

TEST_CASE("An established fleet never yields to an older solitary machine, by seed or by beacon",
          "[node][formation][fleet][seed]")
{
    // Two routes to the same meeting. Through seeds only, the fleet's leader never hears of the old
    // machine at all -- a member beat probes no seed (`TickMember` decides over beacons alone) -- so
    // what keeps the fleet is that structure, and what the case shows is that the OLD machine is the
    // one that asks. On the LAN the leader does hear it, and the encounter row is what keeps the fleet.
    enum class Route : std::uint8_t
    {
        SeedsOnly,
        Lan,
    };
    for (auto const route: { Route::SeedsOnly, Route::Lan })
    {
        INFO((route == Route::SeedsOnly ? "through seeds only" : "on the LAN"));
        core::platform::ManualClock clock;
        core::platform::ManualWallClock wall;
        Lan lan { clock, wall };
        lan.AddMachine("n-office", 500);
        lan.AddMachine("n-laptop", 600);
        lan.RunFor(Lan::MeetWithin);
        lan.Approve("n-office", "n-laptop");
        lan.RunFor(Adopted);
        REQUIRE(lan.ModeOf("n-laptop") == Cluster::NodeMode::Learner);
        auto const fleet = lan.ClusterOf("n-office");

        // Minted long before the fleet.
        lan.AddMachine("n-old", 100);
        if (route == Route::SeedsOnly)
        {
            lan.SetOnOtherSubnet("n-old");
            lan.Seed("n-office", "n-old");
            lan.Seed("n-old", "n-office");
        }
        lan.RunFor(Lan::MeetWithin + std::chrono::duration_cast<std::chrono::seconds>(SeedProbeInterval));
        INFO("the office said:\n" << lan.LogOf("n-office") << "the old machine said:\n" << lan.LogOf("n-old"));
        CHECK(lan.ModeOf("n-office") == Cluster::NodeMode::Voter);
        CHECK(lan.ClusterOf("n-office") == fleet);
        CHECK(lan.ClusterOf("n-laptop") == fleet);
        CHECK(lan.ModeOf("n-old") == Cluster::NodeMode::Pending);
    }
}

TEST_CASE("A rejected machine goes back to solitary and does not ask that fleet again for an hour",
          "[node][formation][fleet][reject]")
{
    core::platform::ManualClock clock;
    core::platform::ManualWallClock wall;
    Lan lan { clock, wall };
    lan.AddMachine("n-office", 100);
    lan.AddMachine("n-laptop", 500);
    lan.RunFor(Lan::MeetWithin);
    REQUIRE(lan.ModeOf("n-laptop") == Cluster::NodeMode::Pending);

    lan.Reject("n-office", "n-laptop");
    lan.RunFor(Adopted);
    INFO("the laptop said:\n" << lan.LogOf("n-laptop"));
    CHECK(lan.ModeOf("n-laptop") == Cluster::NodeMode::Solitary);
    CHECK(lan.ClusterOf("n-laptop") != lan.ClusterOf("n-office"));

    // The office keeps beaconing all along; the refusal is what keeps the laptop from asking...
    auto const remembered = std::chrono::duration_cast<std::chrono::seconds>(RejectedRetryAfter);
    lan.RunFor(remembered - Adopted - Lan::MeetWithin);
    CHECK(lan.ModeOf("n-laptop") == Cluster::NodeMode::Solitary);
    // ...and only for as long as it is remembered.
    lan.RunFor(2 * Lan::MeetWithin);
    CHECK(lan.ModeOf("n-laptop") == Cluster::NodeMode::Pending);
}

TEST_CASE("Discovery authenticates a learner admitted after the member's own body started",
          "[node][formation][fleet][roster]")
{
    core::platform::ManualClock clock;
    core::platform::ManualWallClock wall;
    Lan lan { clock, wall };
    lan.AddMachine("n-office", 100);
    lan.AddMachine("n-laptop", 500);
    lan.AddMachine("n-tablet", 600);
    lan.RunFor(Lan::MeetWithin);
    lan.Approve("n-office", "n-laptop");
    lan.RunFor(Adopted);
    REQUIRE(lan.ModeOf("n-laptop") == Cluster::NodeMode::Learner);

    // The laptop's body was built before the tablet was admitted, and admitting another learner
    // rebuilds nothing on it: only the applied state reaching its roster can vouch for the tablet.
    lan.Approve("n-office", "n-tablet");
    lan.RunFor(Adopted);
    REQUIRE(lan.ModeOf("n-tablet") == Cluster::NodeMode::Learner);
    lan.RunFor(Lan::MeetWithin);
    auto const peers = lan.AuthenticatedPeersOf("n-laptop");
    INFO("the laptop authenticated " << peers.size() << " peer(s)");
    CHECK(std::ranges::contains(peers, std::string { "n-tablet" }));
}

TEST_CASE("A move whose shape the startup rules refuse is not taken, and the node says which rule",
          "[node][formation][fleet][judge]")
{
    // `--dashboard` needs a node that serves the scheduler: the laptop's solitary and pending shapes do,
    // and the learner it is admitted as does not. A restart would refuse that shape, so the MOVE is
    // judged before its record is written: the laptop stays pending, keeps serving, and raises
    // `formation-move-refused` naming the rule -- rather than record a shape it cannot serve.
    core::platform::ManualClock clock;
    core::platform::ManualWallClock wall;
    Lan lan { clock, wall };
    lan.AddMachine("n-office", 100);
    lan.AddMachine("n-laptop", 500, [](NodeConfig& cfg) {
        cfg.dashboard = true;
        cfg.adminListen = "127.0.0.1:9100";
    });
    lan.RunFor(Lan::MeetWithin);
    REQUIRE(lan.ModeOf("n-laptop") == Cluster::NodeMode::Pending);
    REQUIRE(lan.IsServing("n-laptop"));

    lan.Approve("n-office", "n-laptop");
    lan.RunFor(Adopted);
    INFO("the laptop said:\n" << lan.LogOf("n-laptop"));
    CHECK(lan.RecordedModeOf("n-laptop") == Cluster::NodeMode::Pending);
    CHECK(lan.ModeOf("n-laptop") == Cluster::NodeMode::Pending);
    CHECK(lan.IsServing("n-laptop"));
    CHECK_FALSE(lan.RefusalOf("n-laptop").has_value());
    CHECK(lan.PublishedBy("n-laptop").empty());
    auto const& conditions = lan.ConditionsOf("n-laptop");
    REQUIRE(conditions.StateOf(NodeCondition::FormationMoveRefused) == Wire::ConditionState::Raised);
    auto const detail = Testing::DetailOf(conditions, NodeCondition::FormationMoveRefused);
    CHECK(detail.contains("staying pending rather than move to learner"));
    CHECK(detail.contains("--dashboard needs a node that serves"));
}

TEST_CASE("A reformed body runs by what its reform published", "[node][formation][fleet][reform]")
{
    core::platform::ManualClock clock;
    core::platform::ManualWallClock wall;
    Lan lan { clock, wall };
    lan.AddMachine("n-office", 100);
    lan.AddMachine("n-laptop", 500);
    lan.RunFor(Lan::MeetWithin);
    lan.Approve("n-office", "n-laptop");
    lan.RunFor(Adopted);
    REQUIRE(lan.ModeOf("n-laptop") == Cluster::NodeMode::Learner);
    auto const& published = lan.PublishedBy("n-laptop");
    REQUIRE_FALSE(published.empty());
    auto const& last = published.back();
    REQUIRE(last.formation.has_value());
    CHECK(Unwrap(last.formation).mode == Cluster::NodeMode::Learner);
    CHECK(Unwrap(last.formation).clusterId == lan.ClusterOf("n-office"));
}

TEST_CASE("A machine resumes its recorded mode after a restart in every mode", "[node][formation][fleet]")
{
    core::platform::ManualClock clock;
    core::platform::ManualWallClock wall;
    Lan lan { clock, wall };
    lan.AddMachine("n-office", 100);
    lan.AddMachine("n-laptop", 500);

    lan.Restart("n-laptop");
    CHECK(lan.ModeOf("n-laptop") == Cluster::NodeMode::Solitary);
    lan.RunFor(Lan::MeetWithin);
    REQUIRE(lan.ModeOf("n-laptop") == Cluster::NodeMode::Pending);
    lan.Restart("n-laptop");
    CHECK(lan.ModeOf("n-laptop") == Cluster::NodeMode::Pending);
    lan.Approve("n-office", "n-laptop");
    lan.RunFor(Adopted);
    REQUIRE(lan.ModeOf("n-laptop") == Cluster::NodeMode::Learner);
    auto const fleet = lan.ClusterOf("n-laptop");
    lan.Restart("n-laptop");
    CHECK(lan.ModeOf("n-laptop") == Cluster::NodeMode::Learner);
    CHECK(lan.ClusterOf("n-laptop") == fleet);
    lan.Restart("n-office");
    CHECK(lan.ModeOf("n-office") == Cluster::NodeMode::Voter);
    CHECK(lan.ClusterOf("n-office") == fleet);
}

TEST_CASE("A forgotten learner returns to solitary under a new cluster and does not rejoin by itself",
          "[node][formation][fleet]")
{
    core::platform::ManualClock clock;
    core::platform::ManualWallClock wall;
    Lan lan { clock, wall };
    lan.AddMachine("n-office", 100);
    lan.AddMachine("n-laptop", 500);
    lan.RunFor(Lan::MeetWithin);
    lan.Approve("n-office", "n-laptop");
    lan.RunFor(Adopted);
    REQUIRE(lan.ModeOf("n-laptop") == Cluster::NodeMode::Learner);

    // The forget is told to the office alone; the laptop hears it from the office's signed refusal.
    lan.Forget("n-office", "n-laptop");
    lan.RunFor(Lan::TickEvery);
    INFO("the laptop said:\n" << lan.LogOf("n-laptop"));
    CHECK(lan.ModeOf("n-laptop") == Cluster::NodeMode::Solitary);
    CHECK(lan.ClusterOf("n-laptop") != lan.ClusterOf("n-office"));
    lan.RunFor(5min);
    CHECK(lan.ModeOf("n-laptop") != Cluster::NodeMode::Learner); // it asks again, and nobody approved it
}

TEST_CASE("Joining records the machine's endpoint and a reload moves it", "[node][formation][fleet][endpoint]")
{
    // What every resolver dials for a machine is the record its fleet keeps: written at the approval
    // from the endpoint the joiner's `Enroll` stated, and moved only by that machine's own proven
    // announcement -- here after an accepted reload of `--advertise`, on the presence loop.
    core::platform::ManualClock clock;
    core::platform::ManualWallClock wall;
    Lan lan { clock, wall };
    lan.AddMachine("n-office", 100);
    lan.AddMachine("n-laptop", 500);
    lan.RunFor(Lan::MeetWithin);
    lan.Approve("n-office", "n-laptop");

    // At the approval itself, before any announcement could have said it.
    CHECK(lan.RecordedEndpointOf("n-office", "n-laptop") == lan.AdvertisedOf("n-laptop"));
    lan.RunFor(Adopted);
    REQUIRE(lan.ModeOf("n-laptop") == Cluster::NodeMode::Learner);
    REQUIRE(lan.ClusterOf("n-laptop") == lan.ClusterOf("n-office"));

    lan.SetAdvertised("n-laptop", "10.9.0.4:6674");
    lan.RunFor(2 * NodeAnnounceInterval);
    CHECK(lan.RecordedEndpointOf("n-office", "n-laptop") == "10.9.0.4:6674");
}

TEST_CASE("A pinned office ignores an older fleet on its LAN and joins the fleet it is pinned to",
          "[node][formation][fleet][pin][security]")
{
    // n-rogue minted first, so on trust on first use the office and the laptop both yield to it -- the
    // attack the pin exists for. Pinned -- the founder to its own cluster, the laptop to the founder's --
    // neither moves towards it, and the laptop joins the office.
    core::platform::ManualClock clock;
    core::platform::ManualWallClock wall;
    auto const pinTo = [](std::string clusterId) {
        return [clusterId = std::move(clusterId)](Node::NodeConfig& cfg) {
            // The office's cluster under the founder's own key: what `fastcache-cli node` prints on it.
            cfg.fleetPin = Cluster::PinnedFleet { .clusterId = clusterId,
                                                  .voterKeys = { Testing::TestKeyPair("n-office").PublicKey() } };
        };
    };

    SECTION("unpinned, the control: every machine yields to the oldest, rogue or not")
    {
        Lan lan { clock, wall };
        lan.AddMachine("n-rogue", 1);
        lan.AddMachine("n-office", 300);
        lan.AddMachine("n-laptop", 500);
        lan.RunFor(Lan::MeetWithin);
        INFO("n-office said:\n" << lan.LogOf("n-office") << "n-laptop said:\n" << lan.LogOf("n-laptop"));
        CHECK(lan.ModeOf("n-office") == Cluster::NodeMode::Pending);
        CHECK(lan.ModeOf("n-laptop") == Cluster::NodeMode::Pending);
        lan.Approve("n-rogue", "n-laptop");
        lan.RunFor(Adopted);
        CHECK(lan.ClusterOf("n-laptop") == "c-n-rogue");
    }
    SECTION("pinned: the rogue is counted and raised, and the laptop joins the office")
    {
        Lan lan { clock, wall };
        lan.AddMachine("n-rogue", 1);
        lan.AddMachine("n-office", 300, pinTo("c-n-office"));
        lan.AddMachine("n-laptop", 500, pinTo("c-n-office"));
        lan.RunFor(Lan::MeetWithin);
        INFO("n-office said:\n" << lan.LogOf("n-office") << "n-laptop said:\n" << lan.LogOf("n-laptop"));
        CHECK(lan.ModeOf("n-office") == Cluster::NodeMode::Solitary);
        REQUIRE(lan.ModeOf("n-laptop") == Cluster::NodeMode::Pending);
        CHECK(lan.CounterOf("n-office", IMetricsSink::Counter::FormationYieldsRefusedPin) > 0);
        CHECK(lan.CounterOf("n-laptop", IMetricsSink::Counter::FormationYieldsRefusedPin) > 0);
        CHECK(lan.ConditionsOf("n-office").StateOf(NodeCondition::ForeignFleetVisible) == Wire::ConditionState::Raised);

        lan.Approve("n-office", "n-laptop");
        lan.RunFor(Adopted);
        CHECK(lan.ModeOf("n-laptop") == Cluster::NodeMode::Learner);
        CHECK(lan.ClusterOf("n-laptop") == "c-n-office");
    }
}

TEST_CASE("A pinned machine older than the founder it is pinned to still ends in the founder's fleet",
          "[node][formation][fleet][pin]")
{
    // n-desk minted first and is pinned to n-office's cluster; n-office is unpinned. The tiebreak sends
    // the office towards the desk, and the desk -- pinned -- towards the office. The desk records
    // nobody, so the office's ask goes unanswered, and the desk's ask is what the office's operator
    // approves: one fleet, the one the pin names.
    core::platform::ManualClock clock;
    core::platform::ManualWallClock wall;
    Lan lan { clock, wall };
    lan.AddMachine("n-desk", 100, [](Node::NodeConfig& cfg) {
        cfg.fleetPin = Cluster::PinnedFleet { .clusterId = "c-n-office",
                                              .voterKeys = { Testing::TestKeyPair("n-office").PublicKey() } };
    });
    lan.AddMachine("n-office", 300);
    lan.RunFor(Lan::MeetWithin);
    INFO("n-office said:\n" << lan.LogOf("n-office") << "n-desk said:\n" << lan.LogOf("n-desk"));
    REQUIRE(lan.ModeOf("n-desk") == Cluster::NodeMode::Pending);

    lan.Approve("n-office", "n-desk");
    lan.RunFor(Adopted);
    CHECK(lan.ModeOf("n-desk") == Cluster::NodeMode::Learner);
    CHECK(lan.ClusterOf("n-desk") == "c-n-office");
    CHECK(lan.ClusterOf("n-office") == "c-n-office");
}

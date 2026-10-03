// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NodeConditions.hpp"

#include <FastCache/Cluster/ProvenFleet.hpp>

#include <chrono>
#include <map>
#include <string>

#include <core/platform/Clock.hpp>

namespace FastCache::Node
{

/// Raises `foreign-fleet-visible` while another ESTABLISHED fleet is being proven on the segment,
/// naming both ids.
///
/// Two established fleets never yield to each other (`Cluster::ClassifyEncounter` answers
/// `ForeignFleet`), so two that can see each other keep building apart and caching twice until an
/// operator decides which machine belongs where -- and that is a condition row, because a log
/// line scrolls away and the operator who needs to know arrives later.
///
/// **What counts as foreign is `ClassifyEncounter`'s answer, asked afresh, never a test of the two
/// states here.** Two clusters that share a member are a split of ONE fleet, which heals by the
/// tiebreak even when both are established; the decision that says so lives in the encounter
/// table, so this watch raises exactly while that table says `ForeignFleet` -- and stops the moment
/// it says otherwise, for a fleet it already recorded as much as for a new one.
///
/// Every proven fleet is forwarded to `next`, whatever this decided: formation reads the same
/// stream, and a watch that swallowed what it did not raise for would starve it.
///
/// Driven from one thread -- the discovery loop, which both proves fleets and ticks -- and reports
/// through `NodeConditions`, which is what every other thread reads.
class ForeignFleetWatch final: public Cluster::IFleetObserver
{
  public:
    /// How long a foreign fleet is remembered after its last proof.
    ///
    /// Several beacon intervals, because a proof needs a beacon, a challenge and an answer to
    /// arrive and any of the three may be lost: clearing sooner would flap the row on a lossy
    /// segment, and a row that flaps reads as fixed half the time.
    static constexpr std::chrono::minutes ForgetAfter { 5 };

    /// Construct, and answer the row: `clear` until a foreign fleet is proven.
    /// @param self What this node says about itself; asked at every decision. Must outlive this.
    /// @param clock When a fleet was proven, and when it is forgotten. Must outlive this.
    /// @param conditions Where the row is raised and cleared. Must outlive this.
    /// @param next Where every proven fleet goes on to. Must outlive this.
    ForeignFleetWatch(Cluster::IFleetSummarySource const& self,
                      core::platform::IClock const& clock,
                      NodeConditions& conditions,
                      Cluster::IFleetObserver& next);

    /// Classify @p fleet against this node, raise or re-say the row, and forward it to `next`.
    /// @param fleet A proven fleet of another cluster.
    void OnFleetProven(Cluster::ProvenFleet const& fleet) override;

    /// Forget fleets not proven for `ForgetAfter`, and any this node no longer classifies as
    /// foreign; clear the row once none is left.
    void Tick();

  private:
    /// One foreign fleet, as last proven.
    struct Seen
    {
        Cluster::ProvenFleet fleet;               ///< What it proved, last time.
        core::platform::SteadyTimePoint provenAt; ///< When.
    };

    /// Whether @p fleet is foreign to this node as it stands now.
    /// @param fleet A proven fleet.
    /// @return True when the encounter table says neither yields.
    [[nodiscard]] bool IsForeign(Cluster::ProvenFleet const& fleet) const;

    /// Raise the row naming every remembered fleet, or clear it when none is.
    void Publish();

    Cluster::IFleetSummarySource const& _self;
    core::platform::IClock const& _clock;
    NodeConditions& _conditions;
    Cluster::IFleetObserver& _next;

    /// Foreign fleets by cluster id; ordered, so the detail names them in one order everywhere.
    /// At most `Cluster::MaxForeignFleets`, the bound the discovery directory holds other fleets to.
    std::map<std::string, Seen> _foreign;
};

} // namespace FastCache::Node

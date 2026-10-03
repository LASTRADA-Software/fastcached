// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NodeConditions.hpp"

#include <FastCache/Cluster/Encounter.hpp>
#include <FastCache/Cluster/FleetPin.hpp>
#include <FastCache/Cluster/ProvenFleet.hpp>
#include <FastCache/Cluster/SplitEvidence.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>

#include <core/platform/Clock.hpp>

namespace FastCache::Node
{

/// Raises `foreign-fleet-visible` while another ESTABLISHED fleet is being proven on the segment
/// that no key this fleet holds proves to be this one split in two, naming both ids -- and
/// `fleet-split-healing` while one that IS proven so is seen: healing by itself, or waiting on an
/// operator where the evidence is only first-use trust (`Cluster::SplitHealing::ByOperator`).
///
/// Two established fleets yield to each other only on split evidence (`Cluster::SplitEvidenceFor`),
/// so two that can see each other with none keep building apart and caching twice until an
/// operator decides which machine belongs where -- and that is a condition row, because a log
/// line scrolls away and the operator who needs to know arrives later. The detail tells apart the
/// two cases an operator reads differently: no machine in common, and a fleet that CLAIMS to record
/// a machine of this one, unproven -- a look-alike somebody minted, or a member list cut short.
///
/// **What counts as foreign or healing is `ClassifyEncounter`'s answer, asked afresh with the
/// evidence read afresh, never a test of the two states here.** So this watch calls a pair healing
/// exactly while there is split evidence for a pair the encounter table would otherwise call
/// `ForeignFleet`, raises foreign for every other `ForeignFleet`, and moves a fleet between the two the
/// moment either changes.
///
/// **A fleet the pin keeps this node out of is raised as foreign too** (`Cluster::Encounter::PinnedElsewhere`):
/// a node that sits alone beside a fleet it would have joined must say why, naming the fleet and the
/// pin, whatever state either side is in -- and that is decided BEFORE a split is, since a fleet the
/// pin refuses is not one this node heals into either.
///
/// Every proven fleet of ANOTHER cluster is forwarded to `next`, whatever this decided: formation reads
/// the same stream, and a watch that swallowed what it did not raise for would starve it. A reply of
/// this node's own fleet is heard here and goes no further.
///
/// Driven from one thread -- the discovery loop, which both proves fleets and ticks -- and reports
/// through `NodeConditions`, which is what every other thread reads.
class ForeignFleetWatch final: public Cluster::IFleetObserver
{
  public:
    /// How long a fleet is remembered after its last proof.
    ///
    /// Several beacon intervals, because a proof needs a beacon, a challenge and an answer to
    /// arrive and any of the three may be lost: clearing sooner would flap the row on a lossy
    /// segment, and a row that flaps reads as fixed half the time.
    static constexpr std::chrono::minutes ForgetAfter { 5 };

    /// The rows this watch answers: what a node that runs consensus and no discovery answers as not
    /// evaluated, and what this clears as it starts.
    static constexpr std::array WatchedConditions { NodeCondition::ForeignFleetVisible, NodeCondition::FleetSplitHealing };

    /// Construct, and answer the rows NOT EVALUATED until there is evidence to clear them on.
    ///
    /// **`clear` says "no other fleet can see this one", and that is only known by having LISTENED.**
    /// So the rows read not-evaluated until discovery has been watched for @p listenFor -- one beacon
    /// interval, by which every machine on the segment has beaconed -- saying when they decide; and
    /// once that has passed, still not-evaluated while no discovery reply has been heard for
    /// `ForgetAfter`, saying so: an empty segment and one whose replies are firewalled look alike
    /// from here, and neither is the evidence `clear` claims. A raised row needs no such wait.
    /// @param self What this node says about itself; asked at every decision. Must outlive this.
    /// @param evidence Whether a proven fleet is this one split; asked at every decision. Must outlive this.
    /// @param clock When a fleet was proven, and when it is forgotten. Must outlive this.
    /// @param conditions Where the rows are raised and cleared. Must outlive this.
    /// @param next Where every proven fleet goes on to. Must outlive this.
    /// @param listenFor How long discovery is watched before an empty table may read `clear`: the
    ///        beacon interval discovery runs at.
    /// @param pin The cluster `--fleet-id` pins this node to, or none: the formation controller's own,
    ///        so the watch and the decision it explains cannot disagree.
    ForeignFleetWatch(Cluster::IFleetSummarySource const& self,
                      Cluster::ISplitEvidenceSource const& evidence,
                      core::platform::IClock const& clock,
                      NodeConditions& conditions,
                      Cluster::IFleetObserver& next,
                      std::chrono::seconds listenFor,
                      Cluster::FleetPin pin);

    /// Classify @p fleet against this node, raise or re-say the rows, and forward it to `next`.
    /// @param fleet A proven fleet of another cluster.
    void OnFleetProven(Cluster::ProvenFleet const& fleet) override;

    /// Forget fleets not proven for `ForgetAfter`, and move any whose verdict changed; re-say the
    /// rows when either set changed.
    void Tick();

  private:
    /// Whether this watch has heard enough to say "nobody": what an empty row reads.
    /// **Private**: never transmitted or persisted.
    enum class Hearing : std::uint8_t
    {
        Listening, ///< Watched for less than `listenFor`, and nothing proven yet: not decided.
        Deaf,      ///< Watched long enough, and no reply heard for `ForgetAfter`: cannot say.
        Heard,     ///< A reply was heard within `ForgetAfter`: an empty table is the truth.
    };

    /// What meeting a proven fleet means for the rows. **Private**: never transmitted or persisted.
    enum class Verdict : std::uint8_t
    {
        Foreign, ///< Neither yields: no evidence proves a split, or the pin keeps this node out.
        Healing, ///< The evidence proves a split: the tiebreak decides who yields, or an operator does.
        Neither, ///< Nothing for either row: a first join, the same fleet, or a solitary side.
    };

    /// One fleet, as last proven and read.
    struct Seen
    {
        Cluster::ProvenFleet fleet;               ///< What it proved, last time.
        core::platform::SteadyTimePoint provenAt; ///< When.
        Cluster::SplitReading reading;            ///< What this node could say about it then.
        /// What the encounter table decided with that reading. Every one is built with the decided
        /// encounter; the default is the one that moves nobody.
        Cluster::Encounter encounter { Cluster::Encounter::ForeignFleet };
    };

    /// Decide @p fleet against this node as it stands now.
    /// @param fleet A proven fleet.
    /// @param provenAt When it was proven.
    /// @return The verdict, and the entry to keep for it.
    [[nodiscard]] std::pair<Verdict, Seen> Decide(Cluster::ProvenFleet const& fleet,
                                                  core::platform::SteadyTimePoint provenAt) const;

    /// File @p seen under @p verdict, taking it out of the other set.
    /// @param verdict Where it belongs.
    /// @param seen The entry.
    void File(Verdict verdict, Seen seen);

    /// Raise both rows naming every remembered fleet, or clear the one with none.
    void Publish();

    /// @param now When it is asked.
    /// @return What this watch has heard, as of @p now.
    [[nodiscard]] Hearing HearingAt(core::platform::SteadyTimePoint now) const;

    /// What an empty row reads, given what was heard: `clear`, or not-evaluated saying why.
    /// @param condition The row.
    void PublishNobody(NodeCondition condition);

    /// Raise or clear `foreign-fleet-visible`.
    /// @param own This node's cluster id.
    void PublishForeign(std::string const& own);

    /// Raise or clear `fleet-split-healing`.
    /// @param own This node's cluster id.
    void PublishHealing(std::string const& own);

    Cluster::IFleetSummarySource const& _self;
    Cluster::ISplitEvidenceSource const& _evidence;
    core::platform::IClock const& _clock;
    NodeConditions& _conditions;
    Cluster::IFleetObserver& _next;

    /// Fleets by cluster id, per row; ordered, so the detail names them in one order everywhere.
    /// Together at most `Cluster::MaxForeignFleets`, the bound the discovery directory holds other
    /// fleets to.
    std::map<std::string, Seen> _foreign;
    std::map<std::string, Seen> _healing;

    std::chrono::seconds _listenFor;                           ///< See the constructor.
    Cluster::FleetPin _pin;                                    ///< See the constructor.
    core::platform::SteadyTimePoint _startedAt;                ///< When this watch began listening.
    std::optional<core::platform::SteadyTimePoint> _lastHeard; ///< When a reply was last proven.
    Hearing _published { Hearing::Listening };                 ///< What the rows last said was heard.
};

} // namespace FastCache::Node

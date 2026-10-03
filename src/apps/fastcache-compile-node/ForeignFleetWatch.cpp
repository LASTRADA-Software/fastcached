// SPDX-License-Identifier: Apache-2.0
#include "ForeignFleetWatch.hpp"

#include <FastCache/Cluster/Encounter.hpp>
#include <FastCache/Cluster/PeerDirectory.hpp>

#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace FastCache::Node
{

namespace
{
    /// Why a foreign fleet does not merge, as its entry in the detail says it.
    ///
    /// "No machine in common" is a statement about the WHOLE list, so it is made only of a whole one: a
    /// fleet recording more machines than its summary carried (`memberTotal`) is said to share none of
    /// the ones it listed, with the cut named -- a machine of this fleet may be among the rest.
    /// @param seen The other fleet's summary, as it was proven.
    /// @param reading What this node could say about it.
    /// @return The reason.
    [[nodiscard]] std::string WhyForeign(CompileCacheWire::FleetSummary const& seen, Cluster::SplitReading const& reading)
    {
        if (!reading.claimedMember.empty())
            return std::format("claims to record {}, not verified by any key this fleet holds, so neither merges",
                               reading.claimedMember);
        if (seen.memberTotal > seen.members.size())
            return std::format("no machine in common among the {} of {} it lists, the rest cut from what it sent",
                               seen.members.size(),
                               seen.memberTotal);
        return "no machine in common";
    }

    /// How a split heals, and why, as its entry in the detail says it: which side yields, for one that
    /// heals by itself; that an operator decides, and on which machine's word, for one that does not.
    /// @param own This node's summary.
    /// @param seen The entry.
    /// @param encounter What this node decided.
    /// @param reading The evidence it decided on.
    /// @return The account.
    [[nodiscard]] std::string HowItHeals(CompileCacheWire::FleetSummary const& own,
                                         CompileCacheWire::FleetSummary const& seen,
                                         Cluster::Encounter encounter,
                                         Cluster::SplitReading const& reading)
    {
        auto const because = Cluster::SplitEvidenceNames[static_cast<std::size_t>(reading.evidence)].name;
        if (Cluster::HealingOf(reading.evidence) != Cluster::SplitHealing::Automatically)
            return std::format("an operator decides, and neither moves on its own (one fleet only on {}'s word: {})",
                               reading.witness,
                               because);
        auto const side = encounter == Cluster::Encounter::Yield ? std::string_view { "this fleet yields to it" }
                                                                 : std::string_view { "it yields to this fleet" };
        return std::format("{} ({}; one fleet because {})", side, Cluster::TieBreakReason(own, seen), because);
    }
} // namespace

ForeignFleetWatch::ForeignFleetWatch(Cluster::IFleetSummarySource const& self,
                                     Cluster::ISplitEvidenceSource const& evidence,
                                     core::platform::IClock const& clock,
                                     NodeConditions& conditions,
                                     Cluster::IFleetObserver& next,
                                     std::chrono::seconds listenFor):
    _self { self },
    _evidence { evidence },
    _clock { clock },
    _conditions { conditions },
    _next { next },
    _listenFor { listenFor },
    _startedAt { clock.now() }
{
    // Answered as the component starts, so neither row reads `undecided` on a node that runs this --
    // and NOT `clear`, which nothing has been listened for yet to say.
    for (auto const condition: WatchedConditions)
        PublishNobody(condition);
}

ForeignFleetWatch::Hearing ForeignFleetWatch::HearingAt(core::platform::SteadyTimePoint now) const
{
    if (_lastHeard.has_value() && now - *_lastHeard < ForgetAfter)
        return Hearing::Heard;
    if (now - _startedAt < _listenFor)
        return Hearing::Listening;
    return Hearing::Deaf;
}

void ForeignFleetWatch::PublishNobody(NodeCondition condition)
{
    auto const now = _clock.now();
    switch (HearingAt(now))
    {
        case Hearing::Heard:
            _conditions.Clear(condition);
            return;
        case Hearing::Listening:
            _conditions.NotEvaluated(
                condition,
                std::format("discovery has been watched for {} of the {} every machine on the segment takes to "
                            "beacon; decided once it has",
                            std::chrono::duration_cast<std::chrono::seconds>(now - _startedAt),
                            _listenFor));
            return;
        case Hearing::Deaf:
            _conditions.NotEvaluated(
                condition,
                _lastHeard.has_value()
                    ? std::format("no discovery reply heard for {}: an empty segment and one whose replies are "
                                  "firewalled look alike from here",
                                  std::chrono::duration_cast<std::chrono::seconds>(now - *_lastHeard))
                    : std::format("no discovery reply heard in the {} since this node began listening: an empty "
                                  "segment and one whose replies are firewalled look alike from here",
                                  std::chrono::duration_cast<std::chrono::seconds>(now - _startedAt)));
            return;
    }
}

std::pair<ForeignFleetWatch::Verdict, ForeignFleetWatch::Seen> ForeignFleetWatch::Decide(
    Cluster::ProvenFleet const& fleet, core::platform::SteadyTimePoint provenAt) const
{
    auto const own = _self.Current();
    auto const reading = _evidence.ReadSplit(fleet.Proven());
    auto const encounter = Cluster::ClassifyEncounter(own, fleet.Proven(), reading.evidence);
    auto seen = Seen { .fleet = fleet, .provenAt = provenAt, .reading = reading, .encounter = encounter };

    // A pair is a split exactly when there is evidence for one and, without it, the encounter table
    // would call the pair foreign -- whether the evidence heals it by the tiebreak or only an operator
    // can. The table and the evidence's own row decide both, so a watch and the controller that acts
    // cannot disagree.
    auto const bare = Cluster::ClassifyEncounter(own, fleet.Proven(), Cluster::SplitEvidence::None);
    if (bare == Cluster::Encounter::ForeignFleet && Cluster::IsSplit(reading.evidence))
        return { Verdict::Healing, std::move(seen) };
    if (encounter == Cluster::Encounter::ForeignFleet)
        return { Verdict::Foreign, std::move(seen) };
    return { Verdict::Neither, std::move(seen) };
}

void ForeignFleetWatch::File(Verdict verdict, Seen seen)
{
    auto const clusterId = seen.fleet.Summary().clusterId;
    auto const held = _foreign.erase(clusterId) + _healing.erase(clusterId) > 0;

    // No longer either -- this node yielded, or a proof names its own cluster: said at once rather
    // than after `ForgetAfter`.
    if (verdict == Verdict::Neither)
        return;

    // Bounded like the directory's table of other fleets, and never evicting: a fleet already held
    // is refreshed, and a new one past the bound is not remembered -- the row is raised already,
    // naming the ones that are.
    if (!held && _foreign.size() + _healing.size() >= Cluster::MaxForeignFleets)
        return;
    auto& into = verdict == Verdict::Foreign ? _foreign : _healing;
    into.emplace(clusterId, std::move(seen));
}

void ForeignFleetWatch::OnFleetProven(Cluster::ProvenFleet const& fleet)
{
    // A reply, whatever it says: what lets an empty table read `clear`.
    _lastHeard = _clock.now();

    // This node's own fleet is evidence of HEARING and nothing more: there is no other fleet in it,
    // and formation has nothing to do with its own -- so it is neither filed nor passed on.
    if (fleet.Summary().clusterId == _self.Current().clusterId)
    {
        if (HearingAt(_clock.now()) != _published)
            Publish();
        return;
    }

    auto [verdict, seen] = Decide(fleet, _clock.now());
    File(verdict, std::move(seen));
    Publish();

    // Whatever this decided: formation reads every proven fleet, the ones this raised for too.
    _next.OnFleetProven(fleet);
}

void ForeignFleetWatch::Tick()
{
    auto const now = _clock.now();
    auto changed = false;

    // Re-decided from what each proved last, with THIS node's summary and evidence as they stand
    // now: a node that yielded, or learned its evidence since, moves the fleet at this tick.
    auto held = std::vector<Seen> {};
    for (auto* const set: { &_foreign, &_healing })
    {
        for (auto& [id, seen]: *set)
            held.push_back(std::move(seen));
        set->clear();
    }
    for (auto& seen: held)
    {
        if (now - seen.provenAt >= ForgetAfter)
        {
            changed = true;
            continue;
        }
        auto [verdict, decided] = Decide(seen.fleet, seen.provenAt);
        changed = changed || decided.encounter != seen.encounter || decided.reading != seen.reading;
        File(verdict, std::move(decided));
    }
    // And when what was HEARD moved -- the listening window passed, or replies stopped -- an empty row
    // says so at this tick rather than at the next proof.
    if (changed || HearingAt(now) != _published)
        Publish();
}

void ForeignFleetWatch::Publish()
{
    _published = HearingAt(_clock.now());
    auto const own = _self.Current().clusterId;
    PublishForeign(own);
    PublishHealing(own);
}

void ForeignFleetWatch::PublishForeign(std::string const& own)
{
    if (_foreign.empty())
    {
        PublishNobody(NodeCondition::ForeignFleetVisible);
        return;
    }

    // The decision the detail asks for names both sides, and the remedy is per MACHINE: which
    // fleet each belongs to, forgotten from the other.
    constexpr auto Remedy = std::string_view { "they will not merge. Decide which one each machine belongs to, and "
                                               "--cluster-forget it from the other" };
    if (_foreign.size() == 1)
    {
        auto const& [id, seen] = *_foreign.begin();
        _conditions.Raise(NodeCondition::ForeignFleetVisible,
                          std::format("this fleet ({}) and another established fleet ({}: {}) can see each other; {}",
                                      own,
                                      id,
                                      WhyForeign(seen.fleet.Summary(), seen.reading),
                                      Remedy));
        return;
    }
    auto others = std::vector<std::string> {};
    others.reserve(_foreign.size());
    for (auto const& [id, seen]: _foreign)
        others.push_back(std::format("{}: {}", id, WhyForeign(seen.fleet.Summary(), seen.reading)));
    _conditions.Raise(NodeCondition::ForeignFleetVisible,
                      ListDetail(std::format("this fleet ({}) and {} other established fleets can see each other; {}. "
                                             "The others:",
                                             own,
                                             others.size(),
                                             Remedy),
                                 others));
}

void ForeignFleetWatch::PublishHealing(std::string const& own)
{
    if (_healing.empty())
    {
        PublishNobody(NodeCondition::FleetSplitHealing);
        return;
    }

    auto const self = _self.Current();
    auto splits = std::vector<std::string> {};
    splits.reserve(_healing.size());
    for (auto const& [id, seen]: _healing)
        splits.push_back(std::format("{}: {}", id, HowItHeals(self, seen.fleet.Summary(), seen.encounter, seen.reading)));
    _conditions.Raise(NodeCondition::FleetSplitHealing,
                      ListDetail(std::format("this fleet ({}) is one fleet split in two with:", own), splits));
}

} // namespace FastCache::Node

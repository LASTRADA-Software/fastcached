// SPDX-License-Identifier: Apache-2.0
#include "ForeignFleetWatch.hpp"

#include <FastCache/Cluster/Encounter.hpp>
#include <FastCache/Cluster/PeerDirectory.hpp>

#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Node
{

ForeignFleetWatch::ForeignFleetWatch(Cluster::IFleetSummarySource const& self,
                                     core::platform::IClock const& clock,
                                     NodeConditions& conditions,
                                     Cluster::IFleetObserver& next):
    _self { self },
    _clock { clock },
    _conditions { conditions },
    _next { next }
{
    // Answered as the component starts, so the row never reads `undecided` on a node that runs
    // this: nothing foreign has been proven yet, which is a checked answer.
    _conditions.Clear(NodeCondition::ForeignFleetVisible);
}

bool ForeignFleetWatch::IsForeign(Cluster::ProvenFleet const& fleet) const
{
    return Cluster::ClassifyEncounter(_self.Current(), fleet.Proven()) == Cluster::Encounter::ForeignFleet;
}

void ForeignFleetWatch::OnFleetProven(Cluster::ProvenFleet const& fleet)
{
    auto const& clusterId = fleet.Summary().clusterId;
    if (IsForeign(fleet))
    {
        // Bounded like the directory's table of other fleets, and never evicting: a fleet already
        // held is refreshed, and a new one past the bound is not remembered -- the row is raised
        // already, naming the ones that are.
        if (auto found = _foreign.find(clusterId); found != _foreign.end())
            found->second = Seen { .fleet = fleet, .provenAt = _clock.now() };
        else if (_foreign.size() < Cluster::MaxForeignFleets)
            _foreign.emplace(clusterId, Seen { .fleet = fleet, .provenAt = _clock.now() });
    }
    else
    {
        // Proven again and no longer foreign -- this node yielded, or the table learned the two
        // share a member: said at once rather than after `ForgetAfter`.
        _foreign.erase(clusterId);
    }
    Publish();

    // Whatever this decided: formation reads every proven fleet, the ones this raised for too.
    _next.OnFleetProven(fleet);
}

void ForeignFleetWatch::Tick()
{
    auto const now = _clock.now();
    auto const before = _foreign.size();
    std::erase_if(_foreign, [&](auto const& entry) {
        return now - entry.second.provenAt >= ForgetAfter || !IsForeign(entry.second.fleet);
    });
    if (_foreign.size() != before)
        Publish();
}

void ForeignFleetWatch::Publish()
{
    if (_foreign.empty())
    {
        _conditions.Clear(NodeCondition::ForeignFleetVisible);
        return;
    }

    auto const own = _self.Current().clusterId;
    auto others = std::vector<std::string> {};
    others.reserve(_foreign.size());
    for (auto const& [id, seen]: _foreign)
        others.push_back(id);

    // The decision the detail asks for names both sides, and the remedy is per MACHINE: which
    // fleet each belongs to, forgotten from the other.
    constexpr auto Remedy = std::string_view { "they will not merge. Decide which one each machine belongs to, and "
                                               "--cluster-forget it from the other" };
    if (others.size() == 1)
    {
        _conditions.Raise(
            NodeCondition::ForeignFleetVisible,
            std::format(
                "this fleet ({}) and another established fleet ({}) can see each other; {}", own, others.front(), Remedy));
        return;
    }
    _conditions.Raise(NodeCondition::ForeignFleetVisible,
                      ListDetail(std::format("this fleet ({}) and {} other established fleets can see each other; {}. "
                                             "The others:",
                                             own,
                                             others.size(),
                                             Remedy),
                                 others));
}

} // namespace FastCache::Node

// SPDX-License-Identifier: Apache-2.0
#include "SchedulerLink.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

namespace FastCache::Node
{

std::optional<SchedulerLink> SchedulerLink::For(std::vector<std::string> configured)
{
    if (configured.empty())
        return std::nullopt;
    return SchedulerLink { std::move(configured) };
}

SchedulerLink::SchedulerLink(std::vector<std::string> configured):
    _configured { std::move(configured) },
    _current { _configured.front() }
{
}

std::string const& SchedulerLink::ConfiguredAt(std::size_t offset) const noexcept
{
    return _configured[(_home + offset) % _configured.size()];
}

void SchedulerLink::BeginRound()
{
    _hops = 0;
    if (_learned.has_value())
    {
        // The walk has not begun: a remembered leader is not one of the configured
        // endpoints, so all of them are still there to fall back to.
        _current = *_learned;
        _walked = 0;
        return;
    }
    _current = ConfiguredAt(0);
    _walked = 1;
}

std::string const& SchedulerLink::Target() const noexcept
{
    return _current;
}

bool SchedulerLink::Redirect(std::string leader)
{
    if (_hops >= MaxAnnounceRedirects)
        // Spent. Reported as "stop" rather than followed anyway, so two schedulers
        // naming each other cost this round and not the thread.
        return false;

    ++_hops;
    _current = std::move(leader);
    return true;
}

void SchedulerLink::Accepted()
{
    // Committed only here, never in `Redirect`: an endpoint some scheduler named
    // is a lead, and an endpoint that took this node's registration is a leader.
    //
    // Arriving back at a configured endpoint FORGETS the remembered one rather
    // than storing it as a remembered leader. Stored, the next round would open
    // there with the whole configured list still untried behind it -- itself
    // included -- so one unreachable endpoint would be dialled twice in a round.
    // It becomes the walk's start instead, so a first entry that was retired is not
    // dialled ahead of it every round.
    if (auto const at = std::ranges::find(_configured, _current); at != _configured.end())
    {
        _home = static_cast<std::size_t>(std::distance(_configured.begin(), at));
        _learned.reset();
    }
    else
        _learned = _current;
}

std::optional<std::string> SchedulerLink::Lost()
{
    // Whatever failed, a remembered leader is no longer one: either it is what just
    // failed, or it answered this round with a redirect and so no longer leads.
    _learned.reset();

    if (_walked >= _configured.size())
        // Every configured endpoint has been tried this round. There is nowhere
        // further back to fall, and retrying one inside the same round would be a
        // spin -- the one that redirected here would name the same leader again.
        return std::nullopt;

    // Try the next configured endpoint now rather than a heartbeat interval from
    // now: this machine is missing from the fleet for as long as this takes, and a
    // configured endpoint is the one still standing after an election the remembered
    // leader lost, or after the machine an earlier entry named was retired.
    _current = ConfiguredAt(_walked);
    ++_walked;
    return _current;
}

} // namespace FastCache::Node

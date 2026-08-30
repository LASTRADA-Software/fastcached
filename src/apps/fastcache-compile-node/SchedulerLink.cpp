// SPDX-License-Identifier: Apache-2.0
#include "SchedulerLink.hpp"

#include <utility>

namespace FastCache::Node
{

SchedulerLink::SchedulerLink(std::string configured):
    _configured { std::move(configured) },
    _current { _configured }
{
}

void SchedulerLink::BeginRound()
{
    _current = _learned.value_or(_configured);
    _hops = 0;
}

std::string const& SchedulerLink::Target() const noexcept
{
    return _current;
}

bool SchedulerLink::Following() const noexcept
{
    return _learned.has_value();
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
    // Arriving back at the configured endpoint FORGETS the remembered one rather
    // than storing it as a value equal to the default. Otherwise a fleet that
    // re-elected back to the original scheduler would keep a `_learned` that is
    // indistinguishable in behaviour and misleading in every diagnostic
    // `Following()` feeds.
    if (_current == _configured)
        _learned.reset();
    else
        _learned = _current;
}

std::optional<std::string> SchedulerLink::Lost()
{
    if (_current == _configured)
        // Already at the endpoint an operator can fix. There is nowhere further
        // back to fall, and retrying it inside the same round would be a spin.
        return std::nullopt;

    // A remembered leader stopped answering. Forget it and try the configured
    // endpoint immediately rather than a heartbeat interval from now: this
    // machine is missing from the fleet for as long as this takes, and the
    // configured endpoint is the one that is still there after an election the
    // remembered leader lost.
    _learned.reset();
    _current = _configured;
    return _current;
}

} // namespace FastCache::Node

// SPDX-License-Identifier: Apache-2.0
#include "SchedulerReachability.hpp"

#include <algorithm>
#include <format>
#include <utility>
#include <vector>

#include <core/Ranges.hpp>

namespace FastCache::Node
{

namespace
{
    /// Whether @p event's cadence has run out over @p elapsed. A row with none never has.
    /// @param event The row whose cadence is asked.
    /// @param elapsed Time since what the cadence is measured from.
    /// @return True when due.
    [[nodiscard]] bool CadenceElapsed(ReachabilityEvent event, core::platform::SteadyDuration elapsed)
    {
        auto const& cadence = ReachabilityRowOf(event).cadence;
        return cadence.has_value() && elapsed >= *cadence;
    }
} // namespace

SchedulerOutcome SchedulerOutcomeOfProof(NodeProofResult result)
{
    if (auto const* const row =
            core::findOrNull(SchedulerOutcomeTable, std::optional { result }, &SchedulerOutcomeRow::proof))
        return row->outcome;
    return SchedulerOutcome::IdentityRefused;
}

RoundReport DescribeSetback(ReachabilityEvent event,
                            SchedulerOutcome outcome,
                            SchedulerFailure const& failure,
                            core::platform::SteadyDuration lasted,
                            LogLevel inherited)
{
    auto const& row = ReachabilityRowOf(event);
    auto const seconds = std::chrono::duration_cast<std::chrono::seconds>(lasted).count();
    auto const sentence = std::vformat(SchedulerOutcomeRowOf(outcome).*row.sentence,
                                       std::make_format_args(failure.endpoint, failure.subject, failure.reason, seconds));
    auto const fallback = failure.next.has_value() ? std::format("; trying {}", *failure.next) : std::string {};
    return RoundReport { .level = row.level.value_or(inherited),
                         .message = std::vformat(row.pattern, std::make_format_args(sentence, seconds, fallback)) };
}

SchedulerReachability::SchedulerReachability(core::platform::IClock const& clock, NodeConditions* conditions):
    _clock { clock },
    _conditions { conditions }
{
    Evaluate();
}

void SchedulerReachability::Evaluate()
{
    if (_conditions == nullptr)
        return;
    for (auto const& row: SchedulerOutcomeTable)
    {
        if (!row.raises.has_value())
            continue;
        auto const raises = *row.raises;
        auto named = std::vector<std::string> {};
        for (auto const& [place, setback]: _setbacks)
            if (SchedulerOutcomeRowOf(setback.outcome).raises == raises)
                named.push_back(place.first);
        if (named.empty())
            _conditions->Clear(raises);
        else
            _conditions->Raise(raises, ListDetail("no answer from ", named));
    }
}

RoundReport SchedulerReachability::Failed(SchedulerOutcome outcome, SchedulerFailure const& failure)
{
    // The clock is read under the lock: both announce loops call in, and a caller that stamped its
    // instant before losing the race would move `touched` and `said` backwards.
    auto const lock = std::scoped_lock { _mutex };
    auto const now = _clock.now();
    Forget(now);

    auto const [at, inserted] =
        _setbacks.try_emplace(Place { std::string { failure.endpoint }, std::string { failure.subject } });
    auto& setback = at->second;

    auto event = ReachabilityEvent::Repeat;
    if (inserted || setback.outcome != outcome)
    {
        // A new setback, or a DIFFERENT one at the same place: either is a transition.
        setback = Setback { .outcome = outcome, .since = now, .said = now, .touched = now, .loudest = LogLevel::Debug };
        event = Open(outcome, failure, now);
    }
    else if (CadenceElapsed(ReachabilityEvent::Reminder, now - setback.said))
        event = ReachabilityEvent::Reminder;
    setback.touched = now;

    // A line above Debug moves the reminder's clock, and raises the level its recovery is said at.
    if (auto const level = ReachabilityRowOf(event).level.value_or(LogLevel::Debug); level > LogLevel::Debug)
    {
        setback.said = now;
        setback.loudest = std::max(setback.loudest, level);
    }
    Evaluate();
    return DescribeSetback(event, outcome, failure, now - setback.since, setback.loudest);
}

std::optional<RoundReport> SchedulerReachability::Succeeded(AnnounceStage stage,
                                                            std::string_view endpoint,
                                                            std::string_view subject)
{
    auto const lock = std::scoped_lock { _mutex };
    auto const now = _clock.now();
    Forget(now);
    Evaluate();

    auto const at = _setbacks.find(Place { std::string { endpoint }, std::string { subject } });
    if (at == _setbacks.end() || SchedulerOutcomeRowOf(at->second.outcome).stage > stage)
        return std::nullopt;

    auto const setback = at->second;
    _setbacks.erase(at);
    Evaluate();
    return DescribeSetback(ReachabilityEvent::Regained,
                           setback.outcome,
                           SchedulerFailure { .endpoint = endpoint, .subject = subject },
                           now - setback.since,
                           setback.loudest);
}

ReachabilityEvent SchedulerReachability::Open(SchedulerOutcome outcome,
                                              SchedulerFailure const& failure,
                                              core::platform::SteadyTimePoint now)
{
    auto opening = Opening { std::string { failure.endpoint }, std::string { failure.subject }, outcome };
    if (auto const opened = _opened.find(opening);
        opened != _opened.end() && !CadenceElapsed(ReachabilityEvent::Relapse, now - opened->second))
        return ReachabilityEvent::Relapse;
    _opened.insert_or_assign(std::move(opening), now);

    if (auto const warned = _warned.find(failure.endpoint);
        warned != _warned.end() && !CadenceElapsed(ReachabilityEvent::Lost, now - warned->second))
        return ReachabilityEvent::Further;
    _warned.insert_or_assign(std::string { failure.endpoint }, now);
    return ReachabilityEvent::Lost;
}

void SchedulerReachability::Forget(core::platform::SteadyTimePoint now)
{
    // STRICTLY longer than a cadence: a place touched exactly a cadence ago is still being asked, and
    // the reminder due at that instant is one this process can honestly give.
    auto const stale = [now](core::platform::SteadyTimePoint at) {
        return (now - at) > SchedulerUnreachableCadence;
    };
    std::erase_if(_setbacks, [&stale](auto const& entry) { return stale(entry.second.touched); });
    std::erase_if(_warned, [&stale](auto const& entry) { return stale(entry.second); });
    std::erase_if(_opened, [&stale](auto const& entry) { return stale(entry.second); });
}

} // namespace FastCache::Node

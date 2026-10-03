// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cluster/DiscoveryBounds.hpp>
#include <FastCache/Core/TokenBucket.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>

#include <core/platform/Clock.hpp>

/// @file WorkBudget.hpp
/// What one kind of discovery's signature work may cost: per source host, then all together.
namespace FastCache::Cluster
{

/// Whether one unit of work may be done, and when not, which budget said no.
///
/// **Private**: never transmitted or persisted.
enum class WorkGrant : std::uint8_t
{
    Granted,     ///< Both budgets had a token; one of each was taken.
    SourceSpent, ///< The source host has spent its own burst; the shared budget was not touched.
    BudgetSpent, ///< The source had a token and took it, but the budget everybody shares is spent.
};

/// A work budget: one row of `WorkBudgets`, over time points the caller passes in.
///
/// Every unit is a signature or a signature check, so a flood of datagrams asking for them is a
/// flood of work this node pays for. Answering a challenge and checking a proof each have one.
///
/// **Per source first, then shared, and the order is the point.** A source that has spent its own
/// burst is refused BEFORE the shared budget is asked, so one host flooding from a real address
/// drains only its own bucket and every other peer's work still comes out of a full shared one.
/// Asked the other way round, each of the flood's datagrams would take a shared token on its way to
/// its own refusal. A flood that varies its source ADDRESS gets a fresh bucket per address and meets
/// the shared budget, which is what bounds the work.
///
/// **The per-source table is bounded** (`MaxBudgetedSources`): a new source past the bound displaces
/// the one heard from LONGEST ago. So a flood that keeps sending keeps its drained bucket, and what
/// it displaces is a quiet host, whose next datagram simply starts from a full bucket again -- a
/// displacement here only ever makes this node MORE willing to work, never less.
///
/// A value over time points, as `TokenBucket` is: the caller passes its injected clock's reading.
class WorkBudget
{
  public:
    /// @param work Which kind of work this budgets: its row of `WorkBudgets`.
    /// @param now When the budget is filled.
    WorkBudget(DiscoveryWork work, core::platform::SteadyTimePoint now) noexcept:
        _row { BudgetOf(work) },
        _shared { _row.burst, _row.refillEvery, now }
    {
    }

    /// Take the tokens one unit of work for @p source costs, when both budgets have one.
    /// @param source The host the datagram came from.
    /// @param now The time of the datagram, from the caller's clock.
    /// @return Which budget, if either, refused.
    [[nodiscard]] WorkGrant TryTake(std::string_view source, core::platform::SteadyTimePoint now)
    {
        auto held = _sources.find(source);
        if (held == _sources.end())
        {
            if (_sources.size() >= MaxBudgetedSources)
                _sources.erase(
                    std::ranges::min_element(_sources, {}, [](auto const& entry) { return entry.second.lastHeard; }));
            held = _sources
                       .emplace(std::string { source },
                                Source { .bucket = TokenBucket { _row.sourceBurst, _row.sourceRefillEvery, now },
                                         .lastHeard = now })
                       .first;
        }
        held->second.lastHeard = now;
        if (!held->second.bucket.TryTake(now))
            return WorkGrant::SourceSpent;
        if (!_shared.TryTake(now))
            return WorkGrant::BudgetSpent;
        return WorkGrant::Granted;
    }

    /// How many source hosts have a budget of their own.
    /// @return The count; never more than `MaxBudgetedSources`.
    [[nodiscard]] std::size_t Sources() const noexcept
    {
        return _sources.size();
    }

  private:
    /// One source host's budget, and when it was last heard from.
    struct Source
    {
        TokenBucket bucket;                        ///< Its own burst and refill.
        core::platform::SteadyTimePoint lastHeard; ///< Its latest datagram; the stalest is displaced first.
    };

    WorkBudgetRow _row; ///< Its limits, copied from `WorkBudgets`.
    TokenBucket _shared;
    /// Ordered, so the stalest of two sources last heard from at the same instant is the same entry
    /// on every standard library.
    std::map<std::string, Source, std::less<>> _sources;
};

} // namespace FastCache::Cluster

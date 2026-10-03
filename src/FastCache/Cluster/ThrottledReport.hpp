// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <utility>

#include <core/platform/Clock.hpp>

/// @file ThrottledReport.hpp
/// A log line said at most once per interval, with how many occurrences it stands for.
namespace FastCache::Cluster
{

/// A report said at most once per interval, and then with how many occurrences it stands for.
///
/// What repeats at a beat, or that a peer can provoke, would otherwise be a line per repetition:
/// discovery's rejected proofs, a beacon refused at every destination, a change of which links
/// refuse one, a failed interface walk. Counting and saying are two calls as well as one, because
/// a caller that counts only a CHANGE still has to say a change it counted while the interval was
/// running, at the next moment it looks -- or the last state before a quiet spell is never said.
class ThrottledReport
{
  public:
    /// @param interval The least time between two reports.
    explicit constexpr ThrottledReport(std::chrono::seconds interval) noexcept:
        _interval { interval }
    {
    }

    /// Count one occurrence, and say whether a report is due.
    /// @param now The time of this occurrence.
    /// @return How many occurrences this report stands for, itself included, when one is due;
    ///         nullopt when it is not.
    [[nodiscard]] std::optional<std::size_t> Note(core::platform::SteadyTimePoint now) noexcept
    {
        Count();
        return Due(now);
    }

    /// Count one occurrence without asking whether it is reported yet.
    void Count() noexcept
    {
        ++_since;
    }

    /// Whether a report of the occurrences counted so far is due.
    /// @param now The time of asking.
    /// @return How many occurrences the report stands for, when at least one is counted and the
    ///         interval since the last report has passed; nullopt otherwise.
    [[nodiscard]] std::optional<std::size_t> Due(core::platform::SteadyTimePoint now) noexcept
    {
        if (_since == 0 || now < _next)
            return std::nullopt;
        _next = now + _interval;
        return std::exchange(_since, 0);
    }

    /// Forget occurrences counted and not yet said: what they described is over, and was said
    /// otherwise. The interval is left as it is.
    void Discard() noexcept
    {
        _since = 0;
    }

  private:
    std::chrono::seconds _interval;
    /// Value-initialized so the first occurrence is always said: the epoch is behind any clock
    /// this runs on, a `ManualClock` never advanced included.
    core::platform::SteadyTimePoint _next {};
    std::size_t _since { 0 };
};

} // namespace FastCache::Cluster

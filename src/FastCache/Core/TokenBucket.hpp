// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <cstddef>

#include <core/platform/Clock.hpp>

namespace FastCache
{

/// A rate limit: at most `capacity` at once, and one more every `refillEvery` after that.
///
/// A value over time points rather than an owner of a clock, so it reads no time of its own: the
/// caller asks its injected `core::platform::IClock` and passes the answer in, which is what lets a
/// `ManualClock` test drive it to the nanosecond. It starts FULL, since a burst is what a rate limit
/// exists to allow, and a clock that stands still or steps back grants nothing rather than
/// underflowing.
class TokenBucket
{
  public:
    /// @param capacity The most tokens held at once, and how many it starts with: the burst.
    /// @param refillEvery How long one token takes to come back: the sustained rate's reciprocal.
    /// @param now When the bucket is filled.
    constexpr TokenBucket(std::size_t capacity,
                          std::chrono::nanoseconds refillEvery,
                          core::platform::SteadyTimePoint now) noexcept:
        _capacity { capacity },
        _refillEvery { refillEvery },
        _tokens { capacity },
        _refilledAt { now }
    {
    }

    /// Take one token, when there is one.
    /// @param now The time of this take, from the caller's clock.
    /// @return True when a token was taken; false when the bucket is empty, taking nothing.
    [[nodiscard]] constexpr bool TryTake(core::platform::SteadyTimePoint now) noexcept
    {
        Refill(now);
        if (_tokens == 0)
            return false;
        --_tokens;
        return true;
    }

  private:
    /// Credit what the time since the last refill earned, never past the capacity.
    /// @param now The caller's time.
    constexpr void Refill(core::platform::SteadyTimePoint now) noexcept
    {
        if (now <= _refilledAt)
            return;
        auto const earned = static_cast<std::size_t>((now - _refilledAt) / _refillEvery);
        if (earned >= _capacity - _tokens)
        {
            _tokens = _capacity;
            _refilledAt = now;
            return;
        }
        _tokens += earned;
        // Advanced by what was earned rather than to `now`, so the part of an interval already
        // elapsed still counts toward the next token: a caller asking every 1.5 intervals is
        // granted two tokens in three intervals, not one.
        _refilledAt += _refillEvery * static_cast<std::chrono::nanoseconds::rep>(earned);
    }

    std::size_t _capacity;
    std::chrono::nanoseconds _refillEvery;
    std::size_t _tokens;
    core::platform::SteadyTimePoint _refilledAt;
};

} // namespace FastCache

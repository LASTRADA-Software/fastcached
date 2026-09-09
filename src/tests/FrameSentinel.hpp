// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <utility>

namespace FastCache::Testing
{

/// What a coroutine-lifetime case counts.
///
/// Atomic because a platform reactor resumes on its own thread, and the same cases
/// run against `TestReactor` and `PlatformReactor` without changing shape.
struct FrameCounters
{
    std::atomic<int> parked { 0 };    ///< Coroutines that reached their suspend point.
    std::atomic<int> completed { 0 }; ///< Coroutine bodies that ran to their end.
    std::atomic<int> destroyed { 0 }; ///< Frames freed, counted by the sentinel each carries.
};

/// A coroutine-frame sentinel: one per frame under test, counted when the frame dies.
///
/// **Shared rather than copied, and that is the rulebook's rule rather than tidiness**:
/// a test fake is a shared helper too, because three private copies of one scripted
/// `ISocket` carried the same defect in two of them. Four private copies of a
/// destruction counter across four test files is the same shape with one more copy.
///
/// **Why a sentinel at all, when LeakSanitizer already reports a leaked frame.** A leak
/// only a sanitizer notices is a red once in N runs on one platform, and reads as a
/// flake; counting destructions fails for the case's own reason everywhere, including
/// the Windows and macOS legs where no leak checker runs at all. The sanitizer stays as
/// the independent second opinion, never as the assertion.
///
/// **Passed BY VALUE into every coroutine**, which is both this project's coroutine rule
/// and what puts the sentinel in the FRAME: a body local would not exist in a lazy
/// `Task` that has never started, and the ownership cases are precisely about such a
/// task. Move-aware, so the caller's temporary dying at the end of the call expression
/// is not counted as the frame dying.
///
/// **It only means something beside a case that must NOT count.** A counter that never
/// observes a frame it expected to survive cannot distinguish *the fix works* from
/// *everything is freed*, and freeing what something else owns is a double free rather
/// than a leak repaired. Every use of this pairs a detached case with an owned one.
class FrameSentinel
{
  public:
    /// @param counters Where the destruction is tallied; never null.
    explicit FrameSentinel(FrameCounters* counters) noexcept:
        _counters { counters }
    {
    }

    FrameSentinel(FrameSentinel&& other) noexcept:
        _counters { std::exchange(other._counters, nullptr) }
    {
    }

    FrameSentinel(FrameSentinel const&) = delete;
    FrameSentinel& operator=(FrameSentinel const&) = delete;
    FrameSentinel& operator=(FrameSentinel&&) = delete;

    ~FrameSentinel()
    {
        if (_counters != nullptr)
            _counters->destroyed.fetch_add(1, std::memory_order_acq_rel);
    }

  private:
    FrameCounters* _counters;
};

} // namespace FastCache::Testing

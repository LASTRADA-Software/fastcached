// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <thread>
#include <utility>

namespace FastCache::Testing
{

/// Run @p call on a thread of its own, and say whether it returned within @p bound -- leaving the
/// thread behind, detached, when it did not.
///
/// For a case whose regression is a HANG inside the call under test: a deadlock, or a wait nothing
/// cancels. Joining such a call turns the red into the ctest timeout, which is not a verdict; this
/// turns it into a failed assertion the case names, by the bound's NAME, in its message.
///
/// **@p call must OWN everything it touches** -- capture `shared_ptr`s, never references into the
/// case's frame -- because a thread left behind outlives the case, and one that borrowed the frame
/// would read freed memory when it woke. It must not throw: nothing on its thread catches. Touches
/// no Catch2 state, so the case asserts on the answer after it returns.
/// @param bound How long the call may take.
/// @param call What to run.
/// @return Whether @p call returned within @p bound.
[[nodiscard]] inline bool ReturnsWithin(std::chrono::milliseconds bound, std::function<void()> call)
{
    auto done = std::make_shared<std::promise<void>>();
    auto returned = done->get_future();
    auto runner = std::thread { [done, call = std::move(call)] {
        call();
        done->set_value();
    } };
    if (returned.wait_for(bound) != std::future_status::ready)
    {
        runner.detach();
        return false;
    }
    runner.join();
    return true;
}

} // namespace FastCache::Testing

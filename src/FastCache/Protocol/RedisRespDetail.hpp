// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <utility>

namespace FastCache::Detail
{

/// Run `fn` and swallow any exception. Used by RedisRespHandler's per-
/// connection ~Cleanup destructor so an implicitly-noexcept destructor's
/// teardown step (e.g. std::scoped_lock raising std::system_error under
/// rare kernel-mutex resource exhaustion) does NOT std::terminate the
/// process before the remaining teardown steps run.
///
/// Logging from here is unsafe: the logger may itself be torn down or
/// take the same lock that just threw. The right behaviour is the
/// destructor's remaining steps continue regardless of one step's
/// failure — the alternative (terminate) leaks the watcher coroutine
/// frame and any stale WATCH index entries until process exit.
///
/// Exposed in a header (rather than buried in an anonymous namespace)
/// so a unit test can pin the swallow contract directly.
/// @tparam Fn Any nullary invokable.
/// @param fn The teardown step.
template <typename Fn>
void SwallowDestructorException(Fn&& fn) noexcept
{
    try
    {
        std::forward<Fn>(fn)();
    }
    catch (...)
    {
        // Deliberately swallowed — see the function's contract above.
        (void) 0;
    }
}

} // namespace FastCache::Detail

// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/IReactor.hpp>

#include <cassert>

namespace FastCache::Detail
{

/// Refuse a teardown that races the reactor's completion dispatch.
///
/// **One assertion, one wording, for every reactor.** The text is what a gate reads
/// and what an operator meeting a crash searches for, so it is a single spelling
/// here rather than one per socket implementation --
/// [#668](https://github.com/LASTRADA-Software/fastcached/issues/668) is in part a
/// story about a rule that existed in exactly one translation unit, inside
/// `#if defined(_WIN32)`, where no analyser and four of five CI legs could reach it.
///
/// `assert` rather than a refusal or a log, and that is the same choice
/// `Detail::ClaimReadSlot` makes for the same reason: the caller has already decided
/// to destroy the object, so there is no answer to return and nothing to recover.
/// What is left is to be loud in the one configuration that can be loud, and to have
/// `reactor-teardown-canary` prove it still is.
/// @param reactor The reactor owning the object about to be destroyed.
inline void AssertTeardownIsSerialisedWithDispatch([[maybe_unused]] IReactor const& reactor) noexcept
{
    assert(reactor.TeardownIsSerialisedWithDispatch()
           && "an object owned by a reactor must be destroyed on that reactor's worker thread, or with that "
              "reactor stopped -- otherwise clearing a pending awaitable races the completion dispatch");
}

} // namespace FastCache::Detail

// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Protocol/RedisMutationObserver.hpp>

#include <FastCache/Protocol/RedisTransaction.hpp>

namespace FastCache
{

RedisMutationObserver::EventDescriptor RedisMutationObserver::DescriptorFor(MutationKind kind) noexcept
{
    // The decorator's role is intentionally narrow (WATCH dirty
    // fan-out only); the descriptor table is kept for future
    // observers that fan out richer events. Returning an empty
    // descriptor here is the contract "no event for this kind".
    static_cast<void>(kind);
    return EventDescriptor {};
}

RedisMutationObserver::RedisMutationObserver(WatchRegistry* watches) noexcept:
    _watches { watches }
{
}

std::size_t RedisMutationObserver::TouchAllForFlush() const noexcept
{
    if (_watches == nullptr)
        return 0;
    return _watches->TouchedAll();
}

void RedisMutationObserver::OnMutation(MutationKind kind, std::string_view key) noexcept
{
    // Single role: fan out the WATCH dirty signal for every successful
    // mutation. Universal across protocols (Redis + memcached) and
    // includes storage-internal events (FlushDb).
    //
    // Keyspace events are intentionally NOT emitted here. Two reasons:
    //   1. The Redis handler layer already publishes verb-specific
    //      keyspace events ("sadd", "srem", "incrby", ...) with
    //      semantic richness the storage layer cannot reproduce. If
    //      this decorator also published, every Redis-mutation would
    //      yield two `__keyevent@0__:*` frames — a visible
    //      behaviour change to subscribers.
    //   2. Memcached has no convention for keyspace events; the
    //      memcached handlers explicitly publish a generic event
    //      where appropriate. Centralising it here would entangle
    //      the storage decorator with Redis semantics.
    //
    // WATCH dirties are safe to double-fire: `MarkDirty` is an
    // idempotent atomic store. The handler-layer
    // `state->watchRegistry->Touched(key)` calls can coexist with this
    // observer without any user-visible effect — the dirty bit is set
    // once and remains set until `ClaimAndClearDirty` consumes it.
    if (kind == MutationKind::FlushDb)
    {
        (void) TouchAllForFlush();
        return;
    }
    if (_watches != nullptr)
        (void) _watches->Touched(key);
}

bool RedisMutationObserver::HasObservers() const noexcept
{
    // Only WATCH dirty fan-out is performed here, so the fast probe
    // tracks the WATCH registry exclusively. Steady-state cost on a
    // daemon with no WATCH'ers is a single relaxed atomic load.
    return (_watches != nullptr) && _watches->HasAnyWatchers();
}

} // namespace FastCache

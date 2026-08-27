// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Protocol/KeyspaceNotifier.hpp>
#include <FastCache/Protocol/RedisMutationObserver.hpp>
#include <FastCache/Protocol/RedisTransaction.hpp>

#include <cstddef>

namespace FastCache
{

namespace
{

    /// One row per `MutationKind`, naming the keyspace event it publishes.
    ///
    /// Most rows are deliberately empty, and that is the interesting part. The
    /// Redis handlers in `RedisResp.cpp` already publish verb-specific events
    /// with a richness the storage layer cannot reproduce (`sadd`, `srem`,
    /// `incrby`, ...), so publishing here as well would put two
    /// `__keyevent@0__:*` frames on the wire for every Redis write.
    ///
    /// `Expire` and `Evict` are the exceptions, because no handler can fire
    /// them: nobody asked for the mutation, so no verb is executing to notice
    /// it. They are the whole reason this observer needs a notifier at all.
    ///
    /// A table rather than a switch, so a new kind is a row. An `EnumTable`
    /// specifically, so the extent comes from the enum and a kind added without
    /// a row fails to compile instead of reading past the end.
    constexpr EnumTable<MutationKind, RedisMutationObserver::EventDescriptor> EventTable { {
        RedisMutationObserver::EventDescriptor { .kind = MutationKind::Set },
        { .kind = MutationKind::Delete },
        { .kind = MutationKind::Append },
        { .kind = MutationKind::Prepend },
        { .kind = MutationKind::Incr },
        { .kind = MutationKind::Touch },
        { .kind = MutationKind::Persist },
        { .kind = MutationKind::MarkStale },
        { .kind = MutationKind::Cas },
        { .kind = MutationKind::Update },
        { .kind = MutationKind::Expire, .name = "expired", .classFlag = KeyspaceEvents::Expired },
        { .kind = MutationKind::Evict, .name = "evicted", .classFlag = KeyspaceEvents::Evicted },
        { .kind = MutationKind::FlushDb },
    } };

    static_assert(RowsInEnumeratorOrder(EventTable,
                                        [](RedisMutationObserver::EventDescriptor const& row) { return row.kind; }));

} // namespace

RedisMutationObserver::EventDescriptor RedisMutationObserver::DescriptorFor(MutationKind kind) noexcept
{
    auto const index = static_cast<std::size_t>(kind);
    if (index >= EventTable.size())
        return EventDescriptor {};
    return EventTable[index];
}

RedisMutationObserver::RedisMutationObserver(WatchRegistry* watches, KeyspaceNotifier* keyspace) noexcept:
    _watches { watches },
    _keyspace { keyspace }
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
    // First role: fan out the WATCH dirty signal for every successful
    // mutation. Universal across protocols (Redis + memcached) and
    // includes storage-internal events (FlushDb, Expire, Evict).
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

    // Second role: publish the keyspace events no protocol handler can. Which
    // kinds those are is the EventTable's answer, not a condition written out
    // here — and for all but Expire and Evict it is "none", because the Redis
    // handlers already publish their own and a second frame per write would be
    // a visible change to every existing subscriber.
    if (_keyspace == nullptr)
        return;
    auto const descriptor = DescriptorFor(kind);
    if (descriptor.name.empty())
        return;
    try
    {
        _keyspace->OnEvent(descriptor.classFlag, descriptor.name, key);
    }
    catch (...)
    {
        // OnEvent formats two channel names and publishes into other
        // connections' buffers, so it allocates — and this is a `noexcept`
        // override reached from `NotifyingStorage::ReclaimDrain`'s destructor,
        // which can run while an exception is already propagating. Letting one
        // escape would end the daemon over a notification. Keyspace events are
        // best-effort in redis too, and `ReclaimLog::Record` guards its own
        // allocation for exactly this reason.
        return;
    }
}

bool RedisMutationObserver::HasObservers() const noexcept
{
    // True if EITHER channel could do something, so the storage decorator's
    // fast-path probe stays precise now that there are two. A daemon with no
    // WATCHers and no subscribers pays two relaxed atomic loads.
    //
    // Both reclaim classes are asked about: the probe gates whether the tiers
    // bother copying a victim's key at all, so answering only for `Expired`
    // would silently stop `evicted` from ever being publishable.
    if (_watches != nullptr && _watches->HasAnyWatchers())
        return true;
    return _keyspace != nullptr
           && (_keyspace->WouldPublish(KeyspaceEvents::Expired) || _keyspace->WouldPublish(KeyspaceEvents::Evicted));
}

} // namespace FastCache

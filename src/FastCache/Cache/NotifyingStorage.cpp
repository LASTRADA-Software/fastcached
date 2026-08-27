// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/IReclaimLog.hpp>
#include <FastCache/Cache/NotifyingStorage.hpp>

#include <utility>
#include <vector>

namespace FastCache
{

NotifyingStorage::NotifyingStorage(IStorage& inner, IStorageMutationObserver* observer) noexcept:
    _inner { inner },
    _observer { observer }
{
}

void NotifyingStorage::Emit(MutationKind kind, std::string_view key) const noexcept
{
    if (_observer == nullptr)
        return;
    // Lock-free fast probe: in the steady state with no WATCHers and no
    // keyspace subscribers, this is a single relaxed atomic load and we
    // skip the per-call virtual dispatch entirely.
    if (!_observer->HasObservers())
        return;
    _observer->OnMutation(kind, key);
}

void NotifyingStorage::Notify(MutationKind kind, std::string_view key) const noexcept
{
    // Whatever the call reclaimed goes out FIRST, because it happened first and
    // because the two can name the same key: an `ADD k` on a lapsed TTL
    // reclaims k inside the lookup and then re-creates it. Reported the other
    // way round a subscriber sees `set k` followed by `expired k` and concludes
    // a live key is gone.
    FlushReclaimed();
    Emit(kind, key);
}

void NotifyingStorage::FlushReclaimed() const noexcept
{
    if (_reclaim == nullptr || !_reclaim->HasPending())
        return;
    // Thread-local, not a member and not a fresh local. A member would be a
    // race -- with `--threads` several reactors call into this decorator at
    // once -- and a fresh local would hand the log a zero-capacity buffer to
    // regrow under the tier's lock on every drain, which on a cache at its byte
    // budget is once per write. Drain swaps, so both sides keep their storage.
    thread_local std::vector<ReclaimedKey> reclaimed;
    _reclaim->Drain(reclaimed);
    // Emit, not Notify: Notify drains first, and this IS the drain.
    for (auto const& entry: reclaimed)
        Emit(entry.kind, entry.key);
}

std::expected<GetResult, StorageError> NotifyingStorage::Get(std::string_view key, TimePoint now)
{
    ReclaimDrain const drain { *this };
    // A Get can consume a lapsed TTL: on the strict-LRU path the lookup erases
    // the entry it finds dead. The tier records that in the reclaim log while
    // it still has the key in hand, and the guard above fires the Expire when
    // this returns — no Peek-before-Get, and no guessing from the result shape.
    return _inner.Get(key, now);
}

std::expected<CasToken, StorageError> NotifyingStorage::Set(std::string_view key,
                                                            std::vector<std::byte> value,
                                                            std::uint32_t flags,
                                                            TimePoint expiry)
{
    ReclaimDrain const drain { *this };
    auto result = _inner.Set(key, std::move(value), flags, expiry);
    if (result.has_value())
        Notify(MutationKind::Set, key);
    return result;
}

std::expected<CasToken, StorageError> NotifyingStorage::Add(
    std::string_view key, std::vector<std::byte> value, std::uint32_t flags, TimePoint expiry, TimePoint now)
{
    ReclaimDrain const drain { *this };
    auto result = _inner.Add(key, std::move(value), flags, expiry, now);
    if (result.has_value())
        Notify(MutationKind::Set, key);
    return result;
}

std::expected<CasToken, StorageError> NotifyingStorage::Replace(
    std::string_view key, std::vector<std::byte> value, std::uint32_t flags, TimePoint expiry, TimePoint now)
{
    ReclaimDrain const drain { *this };
    auto result = _inner.Replace(key, std::move(value), flags, expiry, now);
    if (result.has_value())
        Notify(MutationKind::Set, key);
    return result;
}

std::expected<CasToken, StorageError> NotifyingStorage::Append(std::string_view key,
                                                               std::span<std::byte const> suffix,
                                                               CasToken expected,
                                                               TimePoint now)
{
    ReclaimDrain const drain { *this };
    auto result = _inner.Append(key, suffix, expected, now);
    if (result.has_value())
        Notify(MutationKind::Append, key);
    return result;
}

std::expected<CasToken, StorageError> NotifyingStorage::Prepend(std::string_view key,
                                                                std::span<std::byte const> prefix,
                                                                CasToken expected,
                                                                TimePoint now)
{
    ReclaimDrain const drain { *this };
    auto result = _inner.Prepend(key, prefix, expected, now);
    if (result.has_value())
        Notify(MutationKind::Prepend, key);
    return result;
}

std::expected<CasToken, StorageError> NotifyingStorage::CompareAndSwap(std::string_view key,
                                                                       CasToken expected,
                                                                       std::vector<std::byte> value,
                                                                       std::uint32_t flags,
                                                                       TimePoint expiry,
                                                                       TimePoint now)
{
    ReclaimDrain const drain { *this };
    auto result = _inner.CompareAndSwap(key, expected, std::move(value), flags, expiry, now);
    if (result.has_value())
        Notify(MutationKind::Cas, key);
    return result;
}

std::expected<IStorage::IncrResult, StorageError> NotifyingStorage::IncrementOrInitialize(std::string_view key,
                                                                                          std::uint64_t magnitude,
                                                                                          bool decrement,
                                                                                          TimePoint now)
{
    ReclaimDrain const drain { *this };
    auto result = _inner.IncrementOrInitialize(key, magnitude, decrement, now);
    if (result.has_value())
        Notify(MutationKind::Incr, key);
    return result;
}

std::expected<void, StorageError> NotifyingStorage::Delete(std::string_view key, TimePoint now)
{
    ReclaimDrain const drain { *this };
    auto result = _inner.Delete(key, now);
    if (result.has_value())
        Notify(MutationKind::Delete, key);
    return result;
}

std::expected<CasToken, StorageError> NotifyingStorage::Touch(std::string_view key, TimePoint newExpiry, TimePoint now)
{
    ReclaimDrain const drain { *this };
    auto result = _inner.Touch(key, newExpiry, now);
    if (result.has_value())
        Notify(MutationKind::Touch, key);
    return result;
}

std::expected<GetResult, StorageError> NotifyingStorage::Peek(std::string_view key, TimePoint now)
{
    ReclaimDrain const drain { *this };
    return _inner.Peek(key, now);
}

std::expected<std::optional<TimePoint>, StorageError> NotifyingStorage::PeekExpiry(std::string_view key, TimePoint now)
{
    ReclaimDrain const drain { *this };
    return _inner.PeekExpiry(key, now);
}
std::expected<bool, StorageError> NotifyingStorage::Prefetch(std::string_view key, TimePoint now)
{
    ReclaimDrain const drain { *this };
    return _inner.Prefetch(key, now);
}

std::expected<CasToken, StorageError> NotifyingStorage::MarkStale(std::string_view key,
                                                                  std::optional<TimePoint> newExpiry,
                                                                  TimePoint now)
{
    ReclaimDrain const drain { *this };
    auto result = _inner.MarkStale(key, newExpiry, now);
    if (result.has_value())
        Notify(MutationKind::MarkStale, key);
    return result;
}

std::expected<GetResult, StorageError> NotifyingStorage::GetAndTouch(std::string_view key,
                                                                     TimePoint newExpiry,
                                                                     TimePoint now)
{
    ReclaimDrain const drain { *this };
    auto result = _inner.GetAndTouch(key, newExpiry, now);
    if (result.has_value())
        Notify(MutationKind::Touch, key);
    return result;
}

std::expected<void, StorageError> NotifyingStorage::CompareAndDelete(std::string_view key, CasToken expected, TimePoint now)
{
    ReclaimDrain const drain { *this };
    auto result = _inner.CompareAndDelete(key, expected, now);
    if (result.has_value())
        Notify(MutationKind::Delete, key);
    return result;
}

std::expected<bool, StorageError> NotifyingStorage::ClearExpiry(std::string_view key, TimePoint now)
{
    ReclaimDrain const drain { *this };
    auto result = _inner.ClearExpiry(key, now);
    // Only fire Persist when a TTL was actually cleared (result == true).
    // ClearExpiry returns false when the entry existed but had no TTL —
    // observable as a no-op, so no notification.
    if (result.has_value() && *result)
        Notify(MutationKind::Persist, key);
    return result;
}

std::expected<CasToken, StorageError> NotifyingStorage::Update(
    std::string_view key,
    std::function<std::expected<UpdateOutcome, StorageError>(GetResult const&)> const& fn,
    TimePoint now)
{
    ReclaimDrain const drain { *this };
    // Track the inner outcome by sniffing the callback's UpdateAction.
    // We wrap the callback so we can observe what the Update will do
    // without forcing the inner storage to expose its decision —
    // important because lock-owning decorators (ShardedStorage) hold
    // their per-shard lock across the callback's execution, and the
    // observer (which may take additional locks) must run AFTER the
    // shard lock is released. We capture the decision in a local and
    // fire the notification once the inner Update returns.
    UpdateAction observedAction { UpdateAction::Unchanged };
    bool calledOnce = false;
    auto const wrapped =
        [&fn, &observedAction, &calledOnce](GetResult const& gr) -> std::expected<UpdateOutcome, StorageError> {
        auto outcome = fn(gr);
        if (outcome.has_value())
        {
            observedAction = outcome->action;
            calledOnce = true;
        }
        return outcome;
    };
    auto result = _inner.Update(key, wrapped, now);
    if (!result.has_value() || !calledOnce)
        return result;
    switch (observedAction)
    {
        case UpdateAction::Store:
            Notify(MutationKind::Update, key);
            break;
        case UpdateAction::Delete:
            Notify(MutationKind::Delete, key);
            break;
        case UpdateAction::Unchanged:
            // Read-only outcome — no notification.
            break;
    }
    return result;
}

void NotifyingStorage::FlushWithGeneration(TimePoint effectiveAt)
{
    ReclaimDrain const drain { *this };
    _inner.FlushWithGeneration(effectiveAt);
    // FlushDb is a whole-database event; the key is empty per the
    // observer contract. WATCH semantics: every WATCH'd key must be
    // dirtied. Concrete observers handle the broadcast.
    Notify(MutationKind::FlushDb, std::string_view {});
}

std::size_t NotifyingStorage::PurgeExpired(TimePoint now)
{
    ReclaimDrain const drain { *this };
    // The count this returns is still opaque — it says how many, not which —
    // but the tier no longer has to answer that question through the return
    // value. It names each key in the reclaim log as it sweeps, and the guard
    // above turns those into Expire events when this returns.
    return _inner.PurgeExpired(now);
}

void NotifyingStorage::SetReclaimLog(IReclaimLog* log)
{
    // Kept AND forwarded. This decorator is the drain end of the very log the
    // tiers below record into, so one call wires both halves of the chain and
    // there is no way to connect one without the other.
    _reclaim = log;
    _inner.SetReclaimLog(log);
}

void NotifyingStorage::Resize(std::size_t newMaxBytes)
{
    ReclaimDrain const drain { *this };
    _inner.Resize(newMaxBytes);
}

StorageStats NotifyingStorage::Snapshot() const noexcept
{
    return _inner.Snapshot();
}

TieredStorageStats NotifyingStorage::SnapshotTiers() const noexcept
{
    return _inner.SnapshotTiers();
}

bool NotifyingStorage::SupportsSharedRead() const noexcept
{
    return _inner.SupportsSharedRead();
}

void NotifyingStorage::PromoteOnRead(std::string_view key, TimePoint now)
{
    ReclaimDrain const drain { *this };
    _inner.PromoteOnRead(key, now);
}

} // namespace FastCache

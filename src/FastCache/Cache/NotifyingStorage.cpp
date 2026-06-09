// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/NotifyingStorage.hpp>

#include <utility>

namespace FastCache
{

NotifyingStorage::NotifyingStorage(IStorage& inner, IStorageMutationObserver* observer) noexcept:
    _inner { inner },
    _observer { observer }
{
}

void NotifyingStorage::Notify(MutationKind kind, std::string_view key) const noexcept
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

std::expected<GetResult, StorageError> NotifyingStorage::Get(std::string_view key, TimePoint now)
{
    // Get may trigger lazy TTL expiry inside the inner storage. The
    // inner storage does NOT know about MutationKind::Expire; the
    // decorator infers it from the result shape: if a Peek would have
    // shown the key present but Get reports it absent, the expiry was
    // consumed by this call. We don't have a cheap way to tell from
    // the outside, so we do not synthesise Expire events from Get — it
    // would require a Peek-before-Get that doubles the read cost. The
    // PurgeExpired path below covers eager expiry, and the
    // protocol-handler layer fires Expire from a separate path. (See
    // KeyspaceEvents::Expired notes in KeyspaceNotifier.hpp.)
    return _inner.Get(key, now);
}

std::expected<CasToken, StorageError> NotifyingStorage::Set(std::string_view key,
                                                            std::vector<std::byte> value,
                                                            std::uint32_t flags,
                                                            TimePoint expiry)
{
    auto result = _inner.Set(key, std::move(value), flags, expiry);
    if (result.has_value())
        Notify(MutationKind::Set, key);
    return result;
}

std::expected<CasToken, StorageError> NotifyingStorage::Add(
    std::string_view key, std::vector<std::byte> value, std::uint32_t flags, TimePoint expiry, TimePoint now)
{
    auto result = _inner.Add(key, std::move(value), flags, expiry, now);
    if (result.has_value())
        Notify(MutationKind::Set, key);
    return result;
}

std::expected<CasToken, StorageError> NotifyingStorage::Replace(
    std::string_view key, std::vector<std::byte> value, std::uint32_t flags, TimePoint expiry, TimePoint now)
{
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
    auto result = _inner.IncrementOrInitialize(key, magnitude, decrement, now);
    if (result.has_value())
        Notify(MutationKind::Incr, key);
    return result;
}

std::expected<void, StorageError> NotifyingStorage::Delete(std::string_view key, TimePoint now)
{
    auto result = _inner.Delete(key, now);
    if (result.has_value())
        Notify(MutationKind::Delete, key);
    return result;
}

std::expected<CasToken, StorageError> NotifyingStorage::Touch(std::string_view key,
                                                              TimePoint newExpiry,
                                                              TimePoint now)
{
    auto result = _inner.Touch(key, newExpiry, now);
    if (result.has_value())
        Notify(MutationKind::Touch, key);
    return result;
}

std::expected<GetResult, StorageError> NotifyingStorage::Peek(std::string_view key, TimePoint now)
{
    return _inner.Peek(key, now);
}

std::expected<std::optional<TimePoint>, StorageError> NotifyingStorage::PeekExpiry(std::string_view key,
                                                                                   TimePoint now)
{
    return _inner.PeekExpiry(key, now);
}

std::expected<CasToken, StorageError> NotifyingStorage::MarkStale(std::string_view key,
                                                                  std::optional<TimePoint> newExpiry,
                                                                  TimePoint now)
{
    auto result = _inner.MarkStale(key, newExpiry, now);
    if (result.has_value())
        Notify(MutationKind::MarkStale, key);
    return result;
}

std::expected<GetResult, StorageError> NotifyingStorage::GetAndTouch(std::string_view key,
                                                                     TimePoint newExpiry,
                                                                     TimePoint now)
{
    auto result = _inner.GetAndTouch(key, newExpiry, now);
    if (result.has_value())
        Notify(MutationKind::Touch, key);
    return result;
}

std::expected<void, StorageError> NotifyingStorage::CompareAndDelete(std::string_view key,
                                                                     CasToken expected,
                                                                     TimePoint now)
{
    auto result = _inner.CompareAndDelete(key, expected, now);
    if (result.has_value())
        Notify(MutationKind::Delete, key);
    return result;
}

std::expected<bool, StorageError> NotifyingStorage::ClearExpiry(std::string_view key, TimePoint now)
{
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
    auto const wrapped = [&fn, &observedAction, &calledOnce](GetResult const& gr)
        -> std::expected<UpdateOutcome, StorageError>
    {
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
    _inner.FlushWithGeneration(effectiveAt);
    // FlushDb is a whole-database event; the key is empty per the
    // observer contract. WATCH semantics: every WATCH'd key must be
    // dirtied. Concrete observers handle the broadcast.
    Notify(MutationKind::FlushDb, std::string_view {});
}

std::size_t NotifyingStorage::PurgeExpired(TimePoint now)
{
    // The inner storage does not give us the list of expired keys, so
    // we cannot fire a per-key Expire event from here. The hook is
    // present in the kind enum for a future storage that DOES surface
    // the victim list (e.g. a write-through tier with a TTL log).
    // For now, PurgeExpired is opaque at this layer; lazy expiry
    // observable from Get/Peek is also not fired (same limitation).
    // The TODO.md gap (KeyspaceEvents::Expired) is acknowledged.
    return _inner.PurgeExpired(now);
}

void NotifyingStorage::Resize(std::size_t newMaxBytes)
{
    _inner.Resize(newMaxBytes);
}

StorageStats NotifyingStorage::Snapshot() const noexcept
{
    return _inner.Snapshot();
}

bool NotifyingStorage::SupportsSharedRead() const noexcept
{
    return _inner.SupportsSharedRead();
}

void NotifyingStorage::PromoteOnRead(std::string_view key, TimePoint now)
{
    _inner.PromoteOnRead(key, now);
}

} // namespace FastCache

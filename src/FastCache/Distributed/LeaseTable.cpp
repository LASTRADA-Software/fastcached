// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Distributed/LeaseTable.hpp>

#include <format>
#include <utility>
#include <vector>

namespace FastCache::Distributed
{

LeaseTable::LeaseTable(IClock& clock, std::chrono::milliseconds leaseTimeout) noexcept:
    _clock { clock },
    _leaseTimeout { leaseTimeout }
{
}

bool LeaseTable::IsLive(Entry const& entry, TimePoint now) const noexcept
{
    // Same clamp as WorkerRegistry::IsLive, and for the same reason: a clock moved
    // backwards in a test must not read as an enormous age and expire everything.
    if (now < entry.issuedAt)
        return true;
    return std::chrono::duration_cast<std::chrono::milliseconds>(now - entry.issuedAt) <= _leaseTimeout;
}

std::optional<Lease> LeaseTable::Acquire(std::string_view key, std::string_view workerId)
{
    std::scoped_lock const guard { _mutex };
    auto const now = _clock.Now();
    auto const keyStr = std::string { key };

    if (auto const existing = _tokenByKey.find(keyStr); existing != _tokenByKey.end())
    {
        auto const held = _byToken.find(existing->second);
        if (held != _byToken.end() && IsLive(held->second, now))
            return std::nullopt; // somebody else is already compiling this

        // Stale: the holder never resolved it and the lifetime has run out. Drop
        // both directions before re-issuing -- leaving the token entry behind would
        // leak one map forever, since nothing else ever visits an expired token.
        if (held != _byToken.end())
            _byToken.erase(held);
        _tokenByKey.erase(existing);
    }

    Lease lease { .token = std::format("l{}", _nextToken++), .workerId = std::string { workerId }, .key = keyStr };
    _tokenByKey.emplace(keyStr, lease.token);
    _byToken.emplace(lease.token, Entry { .lease = lease, .issuedAt = now });
    return lease;
}

std::optional<Lease> LeaseTable::Find(std::string_view token) const
{
    std::scoped_lock const guard { _mutex };
    auto const it = _byToken.find(std::string { token });
    if (it == _byToken.end() || !IsLive(it->second, _clock.Now()))
        return std::nullopt;
    return it->second.lease;
}

std::optional<Lease> LeaseTable::Release(std::string_view token)
{
    std::scoped_lock const guard { _mutex };
    auto const it = _byToken.find(std::string { token });
    if (it == _byToken.end())
        return std::nullopt;

    auto lease = it->second.lease;
    // Erase the key index only when it still points AT THIS token. An expired
    // lease's key may already have been re-leased to somebody else, and removing
    // that mapping here would release a live lease held by another client --
    // letting a third client dispatch the same work while the second is running it.
    if (auto const byKey = _tokenByKey.find(lease.key); byKey != _tokenByKey.end() && byKey->second == lease.token)
        _tokenByKey.erase(byKey);
    _byToken.erase(it);
    return lease;
}

std::size_t LeaseTable::ReleaseWorker(std::string_view workerId)
{
    std::scoped_lock const guard { _mutex };

    // Collected first, then erased: erasing while iterating the map invalidates the
    // iterator, and the key index has to be visited per lease anyway.
    std::vector<std::string> tokens;
    for (auto const& [token, entry]: _byToken)
        if (entry.lease.workerId == workerId)
            tokens.push_back(token);

    for (auto const& token: tokens)
    {
        auto const it = _byToken.find(token);
        if (it == _byToken.end())
            continue;
        if (auto const byKey = _tokenByKey.find(it->second.lease.key); byKey != _tokenByKey.end() && byKey->second == token)
            _tokenByKey.erase(byKey);
        _byToken.erase(it);
    }
    return tokens.size();
}

std::size_t LeaseTable::LiveCount() const
{
    std::scoped_lock const guard { _mutex };
    auto const now = _clock.Now();
    std::size_t live = 0;
    for (auto const& [token, entry]: _byToken)
        if (IsLive(entry, now))
            ++live;
    return live;
}

} // namespace FastCache::Distributed

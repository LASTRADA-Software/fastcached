// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Distributed/LeaseTable.hpp>

#include <algorithm>
#include <chrono>
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

void LeaseTable::Forget(std::unordered_map<std::string, Entry>::iterator entry)
{
    // Erase the key index only when it still points AT THIS token. An expired
    // lease's key may already have been re-leased to somebody else, and removing
    // that mapping here would release a live lease held by another client --
    // letting a third client dispatch the same work while the second is running it.
    auto const& lease = entry->second.lease;
    if (auto const byKey = _tokenByKey.find(lease.key); byKey != _tokenByKey.end() && byKey->second == lease.token)
        _tokenByKey.erase(byKey);
    _byToken.erase(entry);
}

std::optional<Lease> LeaseTable::Release(std::string_view token, std::string_view key)
{
    std::scoped_lock const guard { _mutex };
    auto const it = _byToken.find(std::string { token });
    if (it == _byToken.end())
        return std::nullopt;

    // A token that names a different key is not this caller's, so nothing is
    // resolved AND nothing is erased -- erasing would free the lease of whoever
    // legitimately holds this number. The reachable case is a scheduler that
    // restarted: `_nextToken` began again at one, and a client still holding `l3`
    // from the previous instance is describing a lease that no longer exists.
    if (it->second.lease.key != key)
        return std::nullopt;

    // Liveness, not mere presence -- the same rule `Find` and `IsInFlight` follow,
    // and it is what makes a refusal here mean something. An expired token belongs
    // to a job that outlived its lease, and telling that client `Ok` would leave
    // the one fleet condition worth reporting -- a lease timeout shorter than the
    // slowest translation unit -- with nowhere to be observed.
    auto lease = it->second.lease;
    bool const live = IsLive(it->second, _clock.Now());

    // Dropped either way. Nothing else ever visits an expired token except an
    // `Acquire` for the same key, so answering without erasing would leave one
    // entry in each map for every lease a client reported late.
    Forget(it);
    return live ? std::optional { std::move(lease) } : std::nullopt;
}

std::size_t LeaseTable::ReleaseWorker(std::string_view workerId)
{
    std::scoped_lock const guard { _mutex };
    auto const now = _clock.Now();

    // Collected first, then erased: erasing while iterating the map invalidates the
    // iterator, and the key index has to be visited per lease anyway.
    std::vector<std::string> tokens;
    for (auto const& [token, entry]: _byToken)
        if (entry.lease.workerId == workerId)
            tokens.push_back(token);

    std::size_t released = 0;
    for (auto const& token: tokens)
    {
        auto const it = _byToken.find(token);
        if (it == _byToken.end())
            continue;
        // Counted only when it was still suppressing its key. An entry that had
        // already expired is swept here rather than left, but reporting it as a
        // lease this call freed would overstate what dropping the worker achieved.
        if (IsLive(it->second, now))
            ++released;
        Forget(it);
    }
    return released;
}

bool LeaseTable::IsInFlight(std::string_view key) const
{
    std::scoped_lock const guard { _mutex };
    auto const held = _tokenByKey.find(std::string { key });
    if (held == _tokenByKey.end())
        return false;

    // Liveness, not mere presence. An expired entry is left behind until the next
    // `Acquire` for that key sweeps it, so reporting on the map alone would refuse
    // a key forever once one client had abandoned it.
    auto const entry = _byToken.find(held->second);
    return entry != _byToken.end() && IsLive(entry->second, _clock.Now());
}

std::vector<LeaseReport> LeaseTable::LiveLeases(std::size_t limit) const
{
    std::scoped_lock const guard { _mutex };
    // ONE reading for the whole snapshot, not one per entry: asking per lease would
    // let a single listing report ages measured against different instants, and
    // order rows by a clock that moved underneath the sort. `WorkerRegistry` states
    // the same rule where it takes `now` once for a report.
    auto const now = _clock.Now();

    // A pointer and a duration per lease, not a `LeaseReport`: this runs under the
    // SAME lock `Acquire` and `Release` take, so against the thousands of leases a
    // busy fleet holds, a page scrape would otherwise stall dispatch while it
    // copied two strings per lease and sorted all of them. Only what survives the
    // bound is materialised.
    struct Candidate
    {
        Entry const* entry;
        std::chrono::milliseconds age;
    };

    std::vector<Candidate> live;
    for (auto const& [token, entry]: _byToken)
    {
        if (!IsLive(entry, now))
            continue;
        live.push_back(Candidate { .entry = &entry,
                                   .age = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       now < entry.issuedAt ? Duration::zero() : now - entry.issuedAt) });
    }

    // Oldest first, and the key breaks the tie so a snapshot is reproducible: an
    // unordered_map's iteration order is neither stable nor meaningful, and two
    // leases taken in the same tick are ordinary on a wide parallel build.
    //
    // `partial_sort` rather than a full one, for the same reason the candidates are
    // pointers: the answer is identical and the work is bounded by what was asked
    // for rather than by how busy the fleet is.
    auto const keep = std::min(limit, live.size());
    std::ranges::partial_sort(live, live.begin() + static_cast<std::ptrdiff_t>(keep), [](Candidate a, Candidate b) {
        return a.age != b.age ? a.age > b.age : a.entry->lease.key < b.entry->lease.key;
    });
    live.resize(keep);

    std::vector<LeaseReport> out;
    out.reserve(live.size());
    for (auto const& candidate: live)
        out.push_back(LeaseReport {
            .key = candidate.entry->lease.key, .workerId = candidate.entry->lease.workerId, .age = candidate.age });
    return out;
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

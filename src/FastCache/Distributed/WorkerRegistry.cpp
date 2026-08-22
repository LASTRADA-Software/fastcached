// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Distributed/WorkerRegistry.hpp>

#include <algorithm>
#include <format>
#include <ranges>
#include <utility>

namespace FastCache::Distributed
{

WorkerRegistry::WorkerRegistry(IClock& clock, std::chrono::milliseconds heartbeatTimeout) noexcept:
    _clock { clock },
    _heartbeatTimeout { heartbeatTimeout }
{
}

bool WorkerRegistry::IsLive(Entry const& entry, TimePoint now) const noexcept
{
    // `now < lastSeen` is not a paranoia case: the clock is monotonic, but a
    // ManualClock in a test can be set backwards, and treating a negative age as
    // enormous would expire everything. Clamp rather than assume.
    if (now < entry.lastSeen)
        return true;
    return std::chrono::duration_cast<std::chrono::milliseconds>(now - entry.lastSeen) <= _heartbeatTimeout;
}

std::string WorkerRegistry::Register(WorkerRegistration const& registration)
{
    std::scoped_lock const guard { _mutex };
    auto const now = _clock.Now();

    // Keyed on (fingerprint, endpoint), not on a new id each time: a worker that
    // restarts has lost its id but is the same host running the same toolchain.
    // Issuing it a second id would leave the first pointing at a port that is now
    // dead until it expired, and half the leases for that toolchain would be sent
    // there in the meantime.
    auto const existing = std::ranges::find_if(_workers, [&](auto const& pair) {
        return pair.second.info.fingerprint == registration.fingerprint
               && pair.second.info.endpoint == registration.endpoint;
    });
    if (existing != _workers.end())
    {
        existing->second.info.slots = registration.slots;
        existing->second.info.codecs = registration.codecs;
        // Reset to zero rather than kept: a re-registering worker has restarted, so
        // whatever it was running is gone. Carrying the old count forward would
        // make a restarted worker look permanently busy and take it out of rotation
        // until the first heartbeat corrected it.
        existing->second.info.inFlight = 0;
        existing->second.lastSeen = now;
        return existing->second.info.id;
    }

    auto id = std::format("w{}", _nextId++);
    _workers.emplace(id,
                     Entry { .info = WorkerInfo { .id = id,
                                                  .fingerprint = std::string { registration.fingerprint },
                                                  .endpoint = std::string { registration.endpoint },
                                                  .slots = registration.slots,
                                                  .inFlight = 0,
                                                  .codecs = registration.codecs },
                             .lastSeen = now });
    return id;
}

bool WorkerRegistry::Heartbeat(std::string_view workerId, std::uint32_t inFlight)
{
    std::scoped_lock const guard { _mutex };
    auto const it = _workers.find(std::string { workerId });
    if (it == _workers.end())
        return false;

    // The worker's own count wins. The registry's drifts whenever a client dies
    // between taking a lease and sending the job, and only the worker knows what it
    // is actually running; a heartbeat is a correction as much as a liveness signal.
    it->second.info.inFlight = inFlight;
    it->second.lastSeen = _clock.Now();
    return true;
}

std::expected<WorkerInfo, PickError> WorkerRegistry::Pick(std::string_view fingerprint) const
{
    std::scoped_lock const guard { _mutex };
    auto const now = _clock.Now();

    WorkerInfo const* best = nullptr;
    bool sawMatch = false;
    for (auto const& [id, entry]: _workers)
    {
        // Byte-identical, never "compatible". See the header: an over-strict match
        // costs a local compile, an over-loose one produces a wrong object that is
        // then cached for everybody.
        if (entry.info.fingerprint != fingerprint || !IsLive(entry, now))
            continue;
        sawMatch = true;
        if (entry.info.inFlight >= entry.info.slots)
            continue;
        if (best == nullptr || entry.info.inFlight < best->inFlight)
            best = &entry.info;
    }

    if (best != nullptr)
        return *best;
    // Distinguished so the client reports the right thing: "nothing in the fleet
    // has your toolchain" and "the fleet is busy" want different responses from an
    // operator, even though both make this compile local.
    return std::unexpected(sawMatch ? PickError::NoCapacity : PickError::NoWorker);
}

void WorkerRegistry::JobStarted(std::string_view workerId)
{
    std::scoped_lock const guard { _mutex };
    if (auto const it = _workers.find(std::string { workerId }); it != _workers.end())
        ++it->second.info.inFlight;
}

void WorkerRegistry::JobFinished(std::string_view workerId)
{
    std::scoped_lock const guard { _mutex };
    auto const it = _workers.find(std::string { workerId });
    if (it == _workers.end())
        return;
    // Saturating, not wrapping. `inFlight` is unsigned, and a decrement at zero
    // would make a worker look like it had four billion jobs outstanding — which
    // takes it out of rotation permanently and silently. Reaching zero here is not
    // even a bug: a heartbeat can correct the count downwards between a job
    // starting and finishing.
    if (it->second.info.inFlight > 0)
        --it->second.info.inFlight;
}

void WorkerRegistry::Remove(std::string_view workerId)
{
    std::scoped_lock const guard { _mutex };
    _workers.erase(std::string { workerId });
}

std::vector<WorkerInfo> WorkerRegistry::LiveWorkers() const
{
    std::scoped_lock const guard { _mutex };
    auto const now = _clock.Now();

    std::vector<WorkerInfo> out;
    out.reserve(_workers.size());
    for (auto const& [id, entry]: _workers)
        if (IsLive(entry, now))
            out.push_back(entry.info);

    // Sorted so a snapshot is reproducible: this feeds diagnostics and tests, and
    // an unordered_map's iteration order is neither stable nor meaningful.
    std::ranges::sort(out, [](auto const& a, auto const& b) { return a.id < b.id; });
    return out;
}

} // namespace FastCache::Distributed

// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Distributed/WorkerRegistry.hpp>

#include <algorithm>
#include <format>
#include <map>
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
        existing->second.info.slots = OfferableSlots(registration.capacity, registration.slots);
        existing->second.info.capacity = registration.capacity;
        existing->second.info.codecs = registration.codecs;
        // Reset rather than kept, both of them, and for one reason: a re-registering
        // worker has restarted, so whatever it was running is gone and whatever its
        // machine was doing is a reading from before that. Carrying either forward
        // would make a restarted worker look permanently busy and take it out of
        // rotation until the first heartbeat corrected it.
        existing->second.info.inFlight = 0;
        existing->second.info.load = {};
        existing->second.lastSeen = now;
        return existing->second.info.id;
    }

    auto id = std::format("w{}", _nextId++);
    _workers.emplace(id,
                     Entry { .info = WorkerInfo { .id = id,
                                                  .fingerprint = std::string { registration.fingerprint },
                                                  .endpoint = std::string { registration.endpoint },
                                                  .slots = OfferableSlots(registration.capacity, registration.slots),
                                                  .inFlight = 0,
                                                  .capacity = registration.capacity,
                                                  .load = {},
                                                  .codecs = registration.codecs },
                             .lastSeen = now });
    return id;
}

bool WorkerRegistry::Heartbeat(std::string_view workerId, NodeLoad const& load)
{
    std::scoped_lock const guard { _mutex };
    auto const it = _workers.find(std::string { workerId });
    if (it == _workers.end())
        return false;

    // The worker's own count wins. The registry's drifts whenever a client dies
    // between taking a lease and sending the job, and only the worker knows what it
    // is actually running; a heartbeat is a correction as much as a liveness signal.
    it->second.info.inFlight = load.inFlight;
    it->second.info.load = load;
    it->second.lastSeen = _clock.Now();
    return true;
}

namespace
{
    /// Slots a worker could take a job into right now.
    ///
    /// Its registered count reduced by what it last reported about itself, then by
    /// what it is already running. Both steps matter and neither substitutes for the
    /// other: the first is capacity somebody ELSE has taken -- a developer using
    /// their own machine, a filesystem that filled up -- and the second is capacity
    /// this fleet has taken.
    /// @param info The worker.
    /// @return Slots not currently occupied and not withdrawn.
    [[nodiscard]] std::uint32_t FreeSlots(WorkerInfo const& info) noexcept
    {
        auto const usable = AvailableSlots(info.capacity, info.slots, info.load);
        return info.inFlight >= usable ? 0U : usable - info.inFlight;
    }

    /// Whether `candidate` should be preferred over `incumbent`.
    ///
    /// Most free slots wins, and that is a correction rather than a tweak. The
    /// comparison used to be least-*outstanding* in absolute terms, which treats
    /// every worker as an identical box -- so a 64-core server running 8 jobs looked
    /// busier than a 4-core laptop running 2, when the server had 56 slots free and
    /// the laptop had none. Across a fleet of mixed machines, which is the ordinary
    /// case rather than an exotic one, that sends work to the smallest machines
    /// first and leaves the big ones idle.
    ///
    /// Utilization breaks the tie, so between two workers with equal headroom the
    /// less loaded one takes it. That matters where the headroom is equal but the
    /// machines are not: 4 free of 64 and 4 free of 8 can both take a job, and the
    /// second has proportionally more of itself left.
    ///
    /// Compared as a cross-multiplication rather than a ratio, because a double
    /// would make the ordering depend on rounding and two workers that should tie
    /// could swap on a rebuild -- the kind of instability that makes a fixture flaky
    /// for reasons nobody can reproduce.
    /// @param candidate The worker being considered.
    /// @param incumbent The best so far.
    /// @return True when the candidate is the better choice.
    [[nodiscard]] bool PrefersFirst(WorkerInfo const& candidate, WorkerInfo const& incumbent) noexcept
    {
        auto const candidateFree = FreeSlots(candidate);
        auto const incumbentFree = FreeSlots(incumbent);
        if (candidateFree != incumbentFree)
            return candidateFree > incumbentFree;

        // candidateFree/candidate.slots > incumbentFree/incumbent.slots, without
        // the division. Both slot counts are non-zero here because `OfferableSlots`
        // never yields zero -- which is why that guarantee is stated at the function
        // rather than left as something each caller happens to observe. The
        // denominator is the REGISTERED count deliberately: it asks "how much of this
        // machine is left", and a denominator that shrank with the machine's live
        // load would make a busy workstation look proportionally emptier the busier
        // its owner made it.
        return static_cast<std::uint64_t>(candidateFree) * incumbent.slots
               > static_cast<std::uint64_t>(incumbentFree) * candidate.slots;
    }
} // namespace

std::expected<WorkerInfo, PickError> WorkerRegistry::Pick(std::string_view fingerprint) const
{
    std::scoped_lock const guard { _mutex };
    auto const now = _clock.Now();

    WorkerInfo const* best = nullptr;
    bool sawMatch = false;
    bool sawWithdrawn = false;
    for (auto const& [id, entry]: _workers)
    {
        // Byte-identical, never "compatible". See the header: an over-strict match
        // costs a local compile, an over-loose one produces a wrong object that is
        // then cached for everybody.
        if (entry.info.fingerprint != fingerprint || !IsLive(entry, now))
            continue;
        sawMatch = true;
        // Asked through `FreeSlots` rather than against `slots` directly, so a
        // worker whose scratch disk has filled or whose owner is using it is skipped
        // here rather than picked and left to refuse every job it is sent.
        if (FreeSlots(entry.info) == 0)
        {
            // Which of the two refusals this becomes is decided here, per worker,
            // because it is a per-worker fact: slots free on paper and none in
            // practice is a machine doing something else, while none either way is
            // a fleet full of this build's own work.
            sawWithdrawn = sawWithdrawn || entry.info.inFlight < entry.info.slots;
            continue;
        }
        if (best == nullptr || PrefersFirst(entry.info, *best))
            best = &entry.info;
    }

    if (best != nullptr)
        return *best;
    // Three refusals rather than one "no", because they are three different
    // operator problems: a fingerprint nobody serves, a fleet too small, and a
    // fleet whose machines are busy elsewhere. All three end the same way at the
    // client -- compile locally -- so the distinction exists entirely for whoever
    // has to fix it.
    //
    // `Withdrawn` wins over `NoCapacity` when both are true, and that is the useful
    // way round: "some of your machines are unavailable" is actionable today, while
    // "the fleet is small" is a purchase, and reporting the purchase would hide a
    // fleet-wide full disk behind a number that looks like growth.
    if (!sawMatch)
        return std::unexpected(PickError::NoWorker);
    return std::unexpected(sawWithdrawn ? PickError::Withdrawn : PickError::NoCapacity);
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

std::vector<WorkerReport> WorkerRegistry::LiveWorkerReports() const
{
    std::scoped_lock const guard { _mutex };
    auto const now = _clock.Now();

    std::vector<WorkerReport> out;
    out.reserve(_workers.size());
    for (auto const& [id, entry]: _workers)
        if (IsLive(entry, now))
            out.push_back(WorkerReport { .info = entry.info, .heartbeatAge = AgeOf(entry.lastSeen, now) });

    // Sorted for the reason `LiveWorkers()` sorts: an unordered_map's iteration
    // order is neither stable nor meaningful, and this feeds a page an operator
    // reads twice in a row.
    std::ranges::sort(out, [](auto const& a, auto const& b) { return a.info.id < b.info.id; });
    return out;
}

namespace
{
    /// Whether this entry's heartbeat has actually said anything about a cache.
    ///
    /// The tie-break the grouping needs, and it is not cosmetic. `Register`
    /// resets `info.load` to `{}`, because a re-registering worker has restarted
    /// and whatever it last reported is a reading from before that. So a node's
    /// two entries do NOT always agree: one may have just re-registered -- its
    /// heartbeat refused, its load cleared -- while its sibling still holds
    /// figures from the last round. Picking arbitrarily between them reports that
    /// node as having no cache for one heartbeat interval, which is
    /// indistinguishable from a node that has none.
    /// @param cache The entry's reported cache.
    /// @return True when it carries at least one fact.
    [[nodiscard]] bool SaysAnything(NodeCacheLoad const& cache) noexcept
    {
        return cache.hits.has_value() || cache.misses.has_value()
               || std::ranges::any_of(cache.tiers, [](auto const& tier) { return tier.has_value(); });
    }
} // namespace

std::chrono::milliseconds WorkerRegistry::AgeOf(TimePoint lastSeen, TimePoint now) noexcept
{
    // Clamped at zero rather than allowed to go negative, for the reason `IsLive`
    // is written the way it is: a `ManualClock` can legitimately be set backwards
    // in a test, and an unsigned duration would then report an age of several
    // hundred million years rather than "just now".
    if (now <= lastSeen)
        return std::chrono::milliseconds { 0 };
    return std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSeen);
}

std::vector<NodeReport> WorkerRegistry::NodeReports() const
{
    std::scoped_lock const guard { _mutex };
    auto const now = _clock.Now();

    // Keyed on endpoint, because that is what the entries of one machine share --
    // this registry keys workers on (fingerprint, endpoint), so a node with two
    // `--toolchain` flags is two entries describing one machine.
    struct Candidate
    {
        NodeReport report;
        TimePoint lastSeen {};
        bool contributorSaysCache { false };
    };
    std::map<std::string, Candidate> byEndpoint;

    for (auto const& [id, entry]: _workers)
    {
        if (!IsLive(entry, now))
            continue;

        auto const [slot, inserted] = byEndpoint.try_emplace(
            entry.info.endpoint,
            Candidate { .report = NodeReport { .endpoint = entry.info.endpoint,
                                               .fingerprints = { entry.info.fingerprint },
                                               .capacity = entry.info.capacity,
                                               .load = entry.info.load,
                                               .registeredSlots = entry.info.slots,
                                               .fleetJobsInFlight = entry.info.inFlight,
                                               .heartbeatAge = AgeOf(entry.lastSeen, now) },
                        .lastSeen = entry.lastSeen,
                        .contributorSaysCache = SaysAnything(entry.info.load.cache) });
        if (inserted)
            continue;

        auto& held = slot->second;

        // The fields that ADD across a machine's sibling entries. These are the
        // only two: everything else describes the machine and would be counted
        // once per toolchain by a sum.
        held.report.fingerprints.push_back(entry.info.fingerprint);
        held.report.registeredSlots = std::max(held.report.registeredSlots, entry.info.slots);
        held.report.fleetJobsInFlight += entry.info.inFlight;

        // And the machine-wide half, where only ONE entry contributes -- adding
        // them is the double count this whole function exists to prevent -- so
        // which one is a real choice. An entry that has reported a cache beats one
        // that has not, and among those the most recently heard from wins. The
        // order `_workers` happens to iterate in decides nothing.
        auto const saysCache = SaysAnything(entry.info.load.cache);
        auto const better =
            saysCache != held.contributorSaysCache ? saysCache : entry.lastSeen > held.lastSeen;
        if (better)
        {
            held.report.capacity = entry.info.capacity;
            held.report.load = entry.info.load;
            held.report.heartbeatAge = AgeOf(entry.lastSeen, now);
            held.lastSeen = entry.lastSeen;
            held.contributorSaysCache = saysCache;
        }
    }

    std::vector<NodeReport> out;
    out.reserve(byEndpoint.size());
    // `std::map` rather than an unordered one, so the order is the endpoints'
    // and a snapshot is reproducible -- the property `LiveWorkers()` sorts for.
    for (auto& [endpoint, candidate]: byEndpoint)
    {
        // Sorted for the same reason the outer order is: a node's toolchains come
        // out of an unordered_map, and a snapshot whose column order moved between
        // scrapes would read as the fleet changing.
        std::ranges::sort(candidate.report.fingerprints);
        out.push_back(std::move(candidate.report));
    }
    return out;
}

std::vector<NodeCacheReport> WorkerRegistry::NodeCaches() const
{
    // A projection of `NodeReports()` rather than a second traversal, so the rule
    // that decides which of a machine's sibling entries contributes has ONE
    // definition. Two copies of it drift, and the symptom -- a node reporting no
    // cache for one heartbeat interval, intermittently -- is the one this rule was
    // written to prevent in the first place.
    auto const nodes = NodeReports();
    std::vector<NodeCacheReport> out;
    out.reserve(nodes.size());
    for (auto const& node: nodes)
        out.push_back(NodeCacheReport { .endpoint = node.endpoint,
                                        .capacity = node.capacity.cache,
                                        .load = node.load.cache,
                                        .heartbeatAge = node.heartbeatAge });
    return out;
}

} // namespace FastCache::Distributed

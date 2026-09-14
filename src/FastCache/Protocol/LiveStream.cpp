// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/SleepUntil.hpp>
#include <FastCache/Metrics/StatsReadingCodec.hpp>
#include <FastCache/Protocol/LiveStream.hpp>
#include <FastCache/Protocol/SurfaceRefusal.hpp>

#include <algorithm>
#include <format>
#include <iterator>
#include <limits>
#include <ranges>
#include <utility>

namespace FastCache
{

namespace
{
    namespace Wire = CompileCacheWire;

    /// One set a probe carries, and the two events its membership changing is.
    struct LiveSetRow
    {
        std::vector<LiveFact> LiveEventProbe::* facts; ///< Which set.
        Wire::LiveEventKind joined;                    ///< A key that appeared.
        Wire::LiveEventKind left;                      ///< A key that went.
    };

    /// One scalar a probe carries, and the event its changing is.
    struct LiveScalarRow
    {
        std::optional<LiveFact> LiveEventProbe::* fact; ///< Which scalar.
        Wire::LiveEventKind changed;                    ///< What its changing is called.
    };

    /// The sets, in the order their events are reported.
    constexpr std::array LiveSetRows {
        LiveSetRow { .facts = &LiveEventProbe::members,
                     .joined = Wire::LiveEventKind::MemberJoined,
                     .left = Wire::LiveEventKind::MemberLeft },
        LiveSetRow { .facts = &LiveEventProbe::workers,
                     .joined = Wire::LiveEventKind::WorkerRegistered,
                     .left = Wire::LiveEventKind::WorkerLeft },
    };

    /// The scalars, in the order their events are reported.
    constexpr std::array LiveScalarRows {
        LiveScalarRow { .fact = &LiveEventProbe::leadership, .changed = Wire::LiveEventKind::LeadershipChanged },
        LiveScalarRow { .fact = &LiveEventProbe::survey, .changed = Wire::LiveEventKind::SurveyChanged },
        LiveScalarRow { .fact = &LiveEventProbe::enrollment, .changed = Wire::LiveEventKind::EnrollmentChanged },
    };

    /// How many rows name @p kind.
    [[nodiscard]] constexpr std::size_t RowsNaming(Wire::LiveEventKind kind) noexcept
    {
        return static_cast<std::size_t>(std::ranges::count_if(
                   LiveSetRows, [kind](LiveSetRow const& row) { return row.joined == kind || row.left == kind; }))
               + static_cast<std::size_t>(
                   std::ranges::count_if(LiveScalarRows, [kind](LiveScalarRow const& row) { return row.changed == kind; }));
    }

    // Every kind this build implements is some row's, exactly once: a kind no row names is an
    // event the wire promises and no diff can produce, which is the missing-event defect a diff
    // exists to rule out, reached from the table instead of from a hook.
    static_assert(std::ranges::all_of(Wire::KnownLiveEventKinds,
                                      [](Wire::LiveEventKind kind) { return RowsNaming(kind) == 1; }),
                  "every LiveEventKind must be named by exactly one LiveSetRows or LiveScalarRows row");

    /// What a subject's snapshot body is, beyond what the wire table says about it.
    struct LiveSubjectBody
    {
        Wire::LiveSubject subject; ///< Which subject.
        bool statsReading;         ///< Whether the body starts with an `EncodeStatsReading`.
    };

    /// One row per `LiveSubjectTable` row, in its order.
    constexpr std::array<LiveSubjectBody, Wire::LiveSubjectTable.size()> LiveSubjectBodies { {
        { .subject = Wire::LiveSubject::Cache, .statsReading = true },
        { .subject = Wire::LiveSubject::Node, .statsReading = true },
        { .subject = Wire::LiveSubject::Fleet, .statsReading = false },
    } };

    static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, LiveSubjectBodies.size()),
                                      [](std::size_t index) {
                                          return LiveSubjectBodies.at(index).subject
                                                     == Wire::LiveSubjectTable.at(index).subject
                                                 && static_cast<std::size_t>(Wire::LiveSubjectTable.at(index).subject)
                                                        == index;
                                      }),
                  "LiveSubjectBodies must follow LiveSubjectTable row for row, in LiveSubject order");

    /// A subscription past `MaxLiveSubscriptions`.
    constexpr Cc::SurfaceRefusal RefusedAtCapacity { .code = Wire::ErrorCode::EndpointBusy,
                                                     .counter = IMetricsSink::Counter::LiveSubscriptionsRefusedAtCapacity };

    /// A SUBSCRIBE whose fields did not decode.
    constexpr Cc::SurfaceRefusal RefusedMalformed { .code = Wire::ErrorCode::MalformedFrame,
                                                    .counter = IMetricsSink::Counter::LiveSubscriptionsRefusedMalformed };

    /// The tick @p now falls in, on a grid of @p floor.
    [[nodiscard]] std::uint64_t TickOf(TimePoint now, std::chrono::milliseconds floor) noexcept
    {
        return static_cast<std::uint64_t>(now.time_since_epoch() / floor);
    }

    /// When the tick after @p tick begins.
    [[nodiscard]] TimePoint NextTickAt(std::uint64_t tick, std::chrono::milliseconds floor) noexcept
    {
        return TimePoint { std::chrono::duration_cast<Duration>(floor * static_cast<std::int64_t>(tick + 1)) };
    }

    /// Holds one place under `MaxLiveSubscriptions` for as long as a stream runs.
    ///
    /// RAII for `OpenConnectionSlot`'s reason: a stream ends several ways, and the exit that
    /// forgot to release would leave the cap one lower forever while the surface looks healthy.
    class ActiveSubscription
    {
      public:
        explicit ActiveSubscription(std::shared_ptr<std::atomic<std::size_t>> active) noexcept:
            _active { std::move(active) },
            _claimed { _active->fetch_add(1, std::memory_order_acq_rel) < Wire::MaxLiveSubscriptions }
        {
            if (!_claimed)
                _active->fetch_sub(1, std::memory_order_acq_rel);
        }

        ~ActiveSubscription()
        {
            if (_claimed)
                _active->fetch_sub(1, std::memory_order_acq_rel);
        }

        ActiveSubscription(ActiveSubscription const&) = delete;
        ActiveSubscription(ActiveSubscription&&) = delete;
        ActiveSubscription& operator=(ActiveSubscription const&) = delete;
        ActiveSubscription& operator=(ActiveSubscription&&) = delete;

        /// @return Whether this stream holds a place.
        [[nodiscard]] bool Claimed() const noexcept
        {
            return _claimed;
        }

      private:
        std::shared_ptr<std::atomic<std::size_t>> _active; ///< See `LiveStream::_active`.
        bool _claimed;
    };

    /// The terminal reply for an orderly end: `Ok`, carrying nothing.
    [[nodiscard]] std::vector<std::byte> OrderlyEnd()
    {
        return Wire::EncodeReply(Wire::Status::Ok, {});
    }
} // namespace

std::vector<Wire::LiveEventFields> DiffLiveEvents(LiveEventProbe const& before, LiveEventProbe const& after)
{
    std::vector<Wire::LiveEventFields> events;
    auto const byKey = [](LiveFact const& left, LiveFact const& right) {
        return left.key < right.key;
    };
    auto const emit = [&events](Wire::LiveEventKind kind) {
        return [&events, kind](LiveFact const& fact) {
            events.push_back(Wire::LiveEventFields { .kind = kind, .detail = fact.detail });
        };
    };

    for (auto const& row: LiveSetRows)
    {
        std::vector<LiveFact> joined;
        std::ranges::set_difference(after.*row.facts, before.*row.facts, std::back_inserter(joined), byKey);
        std::ranges::for_each(joined, emit(row.joined));

        std::vector<LiveFact> left;
        std::ranges::set_difference(before.*row.facts, after.*row.facts, std::back_inserter(left), byKey);
        std::ranges::for_each(left, emit(row.left));
    }

    for (auto const& row: LiveScalarRows)
        if (auto const& now = after.*row.fact; before.*row.fact != now)
            events.push_back(Wire::LiveEventFields {
                .kind = row.changed,
                .detail = now.transform([](LiveFact const& fact) { return fact.detail; }).value_or(std::string {}) });

    return events;
}

LiveStep DecideLiveStep(LiveCursor& cursor, std::uint64_t tick, bool forced) noexcept
{
    auto const cadence = std::max<std::uint64_t>(cursor.ticksPerCadence, 1);
    if (tick < cursor.nextDue && !forced)
        return LiveStep {};

    LiveStep step { .snapshot = true, .gap = std::nullopt };
    if (tick >= cursor.nextDue)
        if (auto const missed = (tick - cursor.nextDue) / cadence; missed > 0)
            step.gap = Wire::LiveGapFields { .dropped = missed,
                                             .firstTick = cursor.nextDue,
                                             .lastTick = cursor.nextDue + ((missed - 1) * cadence) };
    cursor.nextDue = tick + cadence;
    return step;
}

bool CarriesStatsReading(Wire::LiveSubject subject) noexcept
{
    auto const index = static_cast<std::size_t>(subject);
    return index < LiveSubjectBodies.size() && LiveSubjectBodies.at(index).statsReading;
}

LiveCapture CaptureCacheSubject(IMetricsSink const& metrics, MetricsSnapshot const& snapshot)
{
    return LiveCapture { .body = EncodeStatsReading(CaptureStatsReading(metrics, snapshot)), .probe = {} };
}

CacheLiveStatsSources::CacheLiveStatsSources(IMetricsSink const& metrics,
                                             std::function<MetricsSnapshot()> snapshot,
                                             std::string endpoint) noexcept:
    _metrics { metrics },
    _snapshot { std::move(snapshot) },
    _endpoint { std::move(endpoint) }
{
}

std::optional<LiveCapture> CacheLiveStatsSources::Capture(Wire::LiveSubject subject) const
{
    if (subject != Wire::LiveSubject::Cache)
        return std::nullopt;
    return CaptureCacheSubject(_metrics, _snapshot());
}

LiveStatsSourceSlot::Attachment LiveStatsSourceSlot::Attach(ILiveStatsSources const& sources)
{
    {
        std::unique_lock const guard { _mutex };
        _sources = &sources;
    }
    return Attachment { *this };
}

void LiveStatsSourceSlot::Detach()
{
    std::unique_lock const guard { _mutex };
    _sources = nullptr;
}

std::optional<LiveCapture> LiveStatsSourceSlot::Capture(Wire::LiveSubject subject) const
{
    std::shared_lock const guard { _mutex };
    return _sources != nullptr ? _sources->Capture(subject) : std::nullopt;
}

std::optional<LiveLeadership> LiveStatsSourceSlot::Leadership() const
{
    std::shared_lock const guard { _mutex };
    return _sources != nullptr ? _sources->Leadership() : std::nullopt;
}

std::string LiveStatsSourceSlot::AnsweringEndpoint() const
{
    std::shared_lock const guard { _mutex };
    return _sources != nullptr ? _sources->AnsweringEndpoint() : std::string {};
}

LiveStream::LiveStream(ILiveStatsSources const& sources, IMetricsSink& metrics) noexcept:
    _sources { sources },
    _metrics { metrics }
{
}

std::optional<LiveStream::TickView> LiveStream::Observe(Wire::LiveSubject subject,
                                                        std::uint64_t tick,
                                                        std::uint64_t eventsFrom)
{
    std::scoped_lock const guard { _mutex };
    auto& state = _subjects.at(static_cast<std::size_t>(subject));

    if (!state.tick.has_value() || *state.tick != tick)
    {
        auto capture = _sources.Capture(subject);
        if (!capture.has_value())
            return std::nullopt;

        // The first capture only sets the baseline: a subscriber arriving is not told that every
        // member it can see has just joined.
        if (state.probe.has_value())
            for (auto& event: DiffLiveEvents(*state.probe, capture->probe))
            {
                state.events.push_back(std::move(event));
                state.eventsEnd += 1;
                if (state.events.size() > LiveEventBacklog)
                    state.events.pop_front();
            }
        state.probe = std::move(capture->probe);
        state.body = std::make_shared<std::vector<std::byte> const>(std::move(capture->body));
        state.tick = tick;
        _metrics.Increment(IMetricsSink::Counter::LiveSnapshotsRendered);
    }

    TickView view { .body = state.body, .events = {}, .eventsEnd = state.eventsEnd };
    auto const firstKept = state.eventsEnd - state.events.size();
    auto const from = std::max(eventsFrom, firstKept);
    if (from < state.eventsEnd)
        view.events.assign(std::next(state.events.begin(), static_cast<std::ptrdiff_t>(from - firstKept)),
                           state.events.end());
    return view;
}

std::optional<std::vector<std::byte>> LiveStream::EndedBySink(IPushSink const& sink) const
{
    if (sink.Stopping())
        return OrderlyEnd();

    switch (sink.Activity())
    {
        case PeerActivity::Departed:
            // Nobody left to tell.
            _metrics.Increment(IMetricsSink::Counter::LiveSubscriptionsEndedByClient);
            return std::vector<std::byte> {};
        case PeerActivity::Reset:
            _metrics.Increment(IMetricsSink::Counter::LiveSubscriptionsEndedByReset);
            return std::vector<std::byte> {};
        case PeerActivity::Sent:
            // The peer wants the connection for its next request: end this one in order, and the
            // surface serves what it sent.
            _metrics.Increment(IMetricsSink::Counter::LiveSubscriptionsEndedByClient);
            return OrderlyEnd();
        case PeerActivity::Quiet:
            break;
    }
    return std::nullopt;
}

Task<std::vector<std::byte>> LiveStream::Serve(
    std::span<std::byte const> frame, std::string peer, IPushSink* sink, ILiveGate const* gate, IReactor* reactor)
{
    // The gate's authority, whatever a surface asked at its door: `Serve` is reachable directly.
    if (auto refusal = gate->RefuseWatcher(peer); refusal.has_value())
        co_return *std::move(refusal);

    auto const request = frame.size() >= Wire::RequestHeaderSize
                             ? Wire::DecodeSubscribeRequest(frame.subspan(Wire::RequestHeaderSize))
                             : std::nullopt;
    if (!request.has_value())
        co_return Cc::Refuse(
            _metrics, RefusedMalformed, "a SUBSCRIBE carries a subject this build serves, a cadence and a dashboard token");
    if (auto refusal = gate->Admit(*request, peer); refusal.has_value())
        co_return *std::move(refusal);

    ActiveSubscription const place { _active };
    if (!place.Claimed())
        co_return Cc::Refuse(_metrics,
                             RefusedAtCapacity,
                             std::format("this process already streams to {} watchers", Wire::MaxLiveSubscriptions));

    auto const subject = request->subject;
    auto const floor = Wire::LiveSubjectTable.at(static_cast<std::size_t>(subject)).floor;
    auto const granted = Wire::GrantLiveCadence(subject, request->cadenceMillis);
    auto const hold = std::max(Wire::MinLiveStreamWriteStall, granted * Wire::LiveIdleCadences);
    auto tick = TickOf(reactor->Clock().Now(), floor);

    // The baseline: this subscriber's event cursor starts at NOW, so it is not replayed events
    // from before it arrived.
    auto const baseline = Observe(subject, tick, std::numeric_limits<std::uint64_t>::max());
    if (!baseline.has_value())
        co_return std::vector<std::byte> {}; // The process is stopping.

    _metrics.Increment(IMetricsSink::Counter::LiveSubscriptionsOpened);

    // Every push leaves through the sink, and every failure ends the stream the same way.
    //
    // A write the transport lost is filed by what the read watch saw: a goodbye is a goodbye, and a
    // write that failed with nothing observed on the read side is the peer resetting under it.
    auto const failed = [this, sink](PushOutcome outcome) {
        if (outcome == PushOutcome::Stalled)
            _metrics.Increment(IMetricsSink::Counter::LiveSubscriptionsStalled);
        else if (!sink->Stopping())
            _metrics.Increment(sink->Activity() == PeerActivity::Departed
                                   ? IMetricsSink::Counter::LiveSubscriptionsEndedByClient
                                   : IMetricsSink::Counter::LiveSubscriptionsEndedByReset);
        return std::vector<std::byte> {};
    };
    auto const push = [sink, hold](std::vector<std::byte> payload) {
        return sink->Push(Wire::EncodeReply(Wire::Status::Push, payload), hold);
    };

    if (auto const outcome = co_await push(Wire::EncodeLiveSubscribed(Wire::LiveSubscribedFields {
            .subject = subject,
            .grantedCadenceMillis = static_cast<std::uint32_t>(granted.count()),
            .statsLayout = CarriesStatsReading(subject) ? StatsReadingLayout : std::uint64_t { 0 },
            .endpoint = _sources.AnsweringEndpoint() }));
        outcome != PushOutcome::Delivered)
        co_return failed(outcome);

    LiveCursor cursor { .nextDue = tick,
                        .ticksPerCadence =
                            static_cast<std::uint64_t>((granted + floor - std::chrono::milliseconds { 1 }) / floor) };
    auto eventsFrom = baseline->eventsEnd;

    while (true)
    {
        if (auto end = EndedBySink(*sink); end.has_value())
            co_return *std::move(end);
        // Re-asked every tick, which is the whole defence against removal failing open.
        if (auto end = gate->Recheck(subject, peer); end.has_value())
            co_return *std::move(end);

        tick = TickOf(reactor->Clock().Now(), floor);
        auto const view = Observe(subject, tick, eventsFrom);
        if (!view.has_value())
            co_return OrderlyEnd(); // The sources were detached: this process is stopping.
        eventsFrom = view->eventsEnd;

        for (auto const& event: view->events)
            if (auto const outcome = co_await push(Wire::EncodeLiveEvent(event)); outcome != PushOutcome::Delivered)
                co_return failed(outcome);

        // An event owes the panel the state it describes, on this tick rather than at the cadence.
        auto const step = DecideLiveStep(cursor, tick, !view->events.empty());
        if (step.gap.has_value())
        {
            _metrics.Increment(IMetricsSink::Counter::LiveSnapshotsSkipped, step.gap->dropped);
            if (auto const outcome = co_await push(Wire::EncodeLiveGap(*step.gap)); outcome != PushOutcome::Delivered)
                co_return failed(outcome);
        }
        if (step.snapshot)
            if (auto const outcome = co_await push(Wire::EncodeLiveSnapshot(tick, *view->body));
                outcome != PushOutcome::Delivered)
                co_return failed(outcome);

        // In steps, so a stop is seen within one `LiveStopCheck` rather than one tick.
        auto const wake = NextTickAt(tick, floor);
        while (!sink->Stopping() && reactor->Clock().Now() < wake)
            co_await SleepUntil { .reactor = reactor, .deadline = std::min(wake, reactor->Clock().Now() + LiveStopCheck) };
    }
}

} // namespace FastCache

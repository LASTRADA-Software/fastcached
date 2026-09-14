// SPDX-License-Identifier: Apache-2.0
#include "LiveStatsResponder.hpp"

#include <FastCache/Async/SleepUntil.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Metrics/StatsReadingCodec.hpp>
#include <FastCache/Protocol/SurfaceRefusal.hpp>

#include <algorithm>
#include <iterator>
#include <limits>
#include <ranges>
#include <utility>

namespace FastCache::Node
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
                     .left = Wire::LiveEventKind::WorkerExpired },
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

    /// What this node does about one subject beyond the gate every subject passes.
    struct LiveSubjectServing
    {
        Wire::LiveSubject subject; ///< Which subject.

        /// Streamed only by the leader, and only to a holder of the dashboard credential -- or,
        /// with no token file, to this machine. The fleet map is behind that credential on
        /// `/fleet`, and a follower's registry is a fraction presented as the whole.
        bool fleetGates;

        /// Whether the snapshot body starts with an `EncodeStatsReading`, so `Subscribed` names
        /// the layout a client must decode it in.
        bool carriesStatsReading;
    };

    /// One row per `LiveSubjectTable` row, in its order.
    constexpr std::array<LiveSubjectServing, Wire::LiveSubjectTable.size()> LiveSubjectServingTable { {
        { .subject = Wire::LiveSubject::Cache, .fleetGates = false, .carriesStatsReading = true },
        { .subject = Wire::LiveSubject::Node, .fleetGates = false, .carriesStatsReading = true },
        { .subject = Wire::LiveSubject::Fleet, .fleetGates = true, .carriesStatsReading = false },
    } };

    static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, LiveSubjectServingTable.size()),
                                      [](std::size_t index) {
                                          return LiveSubjectServingTable.at(index).subject
                                                     == Wire::LiveSubjectTable.at(index).subject
                                                 && static_cast<std::size_t>(Wire::LiveSubjectTable.at(index).subject)
                                                        == index;
                                      }),
                  "LiveSubjectServingTable must follow LiveSubjectTable row for row, in LiveSubject order");

    /// A peer refused at subscribe for not being a member.
    constexpr Cc::SurfaceRefusal RefusedNotAMember { .code = Wire::ErrorCode::NotAMember,
                                                     .counter = IMetricsSink::Counter::LiveSubscriptionsRefusedNotAMember };

    /// A running stream whose peer stopped being a member. The same code as the refusal above and
    /// a different event, so a different counter: one is a stranger turned away, the other a
    /// removal acting on a live connection.
    constexpr Cc::SurfaceRefusal Revoked { .code = Wire::ErrorCode::NotAMember,
                                           .counter = IMetricsSink::Counter::LiveSubscriptionsRevoked };

    /// A fleet subscription without the dashboard credential.
    constexpr Cc::SurfaceRefusal RefusedUnauthenticated {
        .code = Wire::ErrorCode::Unauthenticated,
        .counter = IMetricsSink::Counter::LiveSubscriptionsRefusedUnauthenticated,
    };

    /// A subscription past `MaxLiveSubscriptions`.
    constexpr Cc::SurfaceRefusal RefusedAtCapacity { .code = Wire::ErrorCode::EndpointBusy,
                                                     .counter = IMetricsSink::Counter::LiveSubscriptionsRefusedAtCapacity };

    /// A fleet stream whose node stopped leading.
    constexpr Cc::SurfaceRefusal EndedNotLeader { .code = Wire::ErrorCode::NotLeader,
                                                  .counter = IMetricsSink::Counter::LiveSubscriptionsEndedNotLeader };

    /// A SUBSCRIBE whose fields did not decode.
    constexpr Cc::SurfaceRefusal RefusedMalformed { .code = Wire::ErrorCode::MalformedFrame,
                                                    .counter = IMetricsSink::Counter::LiveSubscriptionsRefusedMalformed };

    /// A fleet subscription at a follower, before it streams.
    constexpr Cc::UncountedRefusal NotLeaderAtSubscribe {
        .code = Wire::ErrorCode::NotLeader,
        .rationale = "a redirect rather than an event: a dashboard pointed at any member is told where the leader is "
                     "and follows it, once per dashboard opened, exactly as the scheduler's own NotLeader is uncounted",
    };

    /// A fleet subscription at a node that runs no scheduler.
    constexpr Cc::UncountedRefusal FleetNotServedHere {
        .code = Wire::ErrorCode::DispatchNotPermitted,
        .rationale = "a misdirection a healthy fleet produces whenever a dashboard is pointed at a worker, answered "
                     "with where to go instead; DispatchNotPermitted and not UnimplementedVerb because the fleet is "
                     "served elsewhere, not unimplemented",
    };

    /// A SUBSCRIBE reached through `Answer`, which cannot carry a stream.
    constexpr Cc::UncountedRefusal NotThroughAnswer {
        .code = Wire::ErrorCode::DispatchNotPermitted,
        .rationale = "unreachable through the endpoint, which asks StreamFor before it would call Answer; a caller "
                     "that cannot stream has no subscription to count",
    };

    /// What this surface does about one refusal it may be asked to answer.
    ///
    /// Exactly one of the two is set; see `NodeStatusResponder.cpp`'s identical record for why a
    /// counter and a rationale are the two claims.
    struct RefusalPolicy
    {
        std::optional<IMetricsSink::Counter> counter; ///< What rises, or nothing.
        std::string_view rationale;                   ///< Why nothing rises. Empty exactly when `counter` is set.
    };

    /// Answer a refusal the way its row decided.
    [[nodiscard]] std::vector<std::byte> AnswerRefusal(IMetricsSink& metrics,
                                                       Wire::ErrorCode code,
                                                       RefusalPolicy const& policy,
                                                       std::string_view detail)
    {
        if (policy.counter.has_value())
            return Cc::Refuse(metrics, { .code = code, .counter = *policy.counter }, detail);
        return Cc::RefuseWithoutCounter({ .code = code, .rationale = policy.rationale }, detail);
    }

    /// What this surface does about each pre-payload decision.
    [[nodiscard]] constexpr RefusalPolicy PrePayloadPolicy(Wire::PrePayloadDecision decision) noexcept
    {
        switch (decision)
        {
            case Wire::PrePayloadDecision::PayloadTooLarge:
                // Counted: SUBSCRIBE's `OpTable` row bounds it to `MaxControlPayload`, so a header
                // declaring more came from no client of this tree at any version.
                return { .counter = IMetricsSink::Counter::LiveSubscriptionsRefusedPayloadTooLarge, .rationale = {} };
            case Wire::PrePayloadDecision::UnknownOpcode:
                return { .counter = std::nullopt,
                         .rationale = "MergedResponder routes only the Live family here, and an opcode with no OpTable "
                                      "row belongs to no family, so it is answered at the door" };
            case Wire::PrePayloadDecision::Unauthenticated:
                return { .counter = std::nullopt,
                         .rationale =
                             "AuthRequired() is false here by decision -- the listener's credential is the "
                             "scheduler's -- and DecidePrePayload yields this only for a surface that requires one" };
            case Wire::PrePayloadDecision::Serve:
                break;
        }
        return { .counter = std::nullopt, .rationale = "Serve is not a refusal and the endpoint never asks about it" };
    }

    static_assert(std::ranges::all_of(std::array { Wire::PrePayloadDecision::Serve,
                                                   Wire::PrePayloadDecision::UnknownOpcode,
                                                   Wire::PrePayloadDecision::PayloadTooLarge,
                                                   Wire::PrePayloadDecision::Unauthenticated },
                                      [](Wire::PrePayloadDecision decision) {
                                          auto const policy = PrePayloadPolicy(decision);
                                          return Cc::StatesOneRefusalClaim(policy.counter.has_value(), policy.rationale);
                                      }),
                  "every pre-payload arm must state either a counter or a rationale, and not both");

    /// One row of `EndpointRefusals`.
    struct EndpointRefusalRow
    {
        EndpointRefusal refusal; ///< Which endpoint decision this describes.
        RefusalPolicy policy;    ///< What this surface does about it.
    };

    /// Why neither credential arm counts here.
    constexpr std::string_view CredentialIsTheSchedulersRationale =
        "AUTH is the Session family, which MergedResponder routes to the scheduler; no credential outcome is ever "
        "decided against this surface";

    /// What this surface does about each endpoint-decided refusal.
    constexpr EnumTable<EndpointRefusal, EndpointRefusalRow> EndpointRefusals { {
        { .refusal = EndpointRefusal::InFlightBudget,
          // Counted, for `NodeStatusResponder`'s reason: a dashboard that cannot subscribe is an
          // operator losing the view at the moment the listener is busiest.
          .policy = { .counter = IMetricsSink::Counter::LiveSubscriptionsRefusedEndpointBusy, .rationale = {} } },
        { .refusal = EndpointRefusal::CredentialMalformed,
          .policy = { .counter = std::nullopt, .rationale = CredentialIsTheSchedulersRationale } },
        { .refusal = EndpointRefusal::CredentialRejected,
          .policy = { .counter = std::nullopt, .rationale = CredentialIsTheSchedulersRationale } },
        { .refusal = EndpointRefusal::AnswerDeadline,
          .policy = { .counter = std::nullopt, .rationale = AnswerDeadlineIsTheEndpointsRationale } },
    } };

    static_assert(RowsInEnumeratorOrder(EndpointRefusals, &EndpointRefusalRow::refusal),
                  "EndpointRefusals must hold one row per EndpointRefusal, in enumerator order");

    static_assert(Cc::RowsStateOneRefusalClaim(EndpointRefusals,
                                               [](EndpointRefusalRow const& row) {
                                                   return Cc::RefusalClaim { .counted = row.policy.counter.has_value(),
                                                                             .rationale = row.policy.rationale };
                                               }),
                  "every endpoint refusal row must state either a counter or a rationale, and not both");

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
    /// forgot to release would leave the cap one lower forever while the node looks healthy.
    class ActiveSubscription
    {
      public:
        explicit ActiveSubscription(std::atomic<std::size_t>& active) noexcept:
            _active { active },
            _claimed { active.fetch_add(1, std::memory_order_acq_rel) < Wire::MaxLiveSubscriptions }
        {
            if (!_claimed)
                _active.fetch_sub(1, std::memory_order_acq_rel);
        }

        ~ActiveSubscription()
        {
            if (_claimed)
                _active.fetch_sub(1, std::memory_order_acq_rel);
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
        std::atomic<std::size_t>& _active;
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

LiveStatsResponder::LiveStatsResponder(ILiveStatsSources const& sources,
                                       Distributed::IMembershipOracle const& membership,
                                       AdminCredential dashboard,
                                       IReactor& reactor,
                                       IMetricsSink& metrics) noexcept:
    _sources { sources },
    _membership { membership },
    _dashboard { std::move(dashboard) },
    _reactor { reactor },
    _metrics { metrics }
{
}

Task<std::vector<std::byte>> LiveStatsResponder::Answer(std::span<std::byte const> frame, std::string peer)
{
    auto const header = Wire::DecodeRequestHeader(frame);
    auto const opRaw = header.has_value() ? header->opRaw : std::uint8_t { 0xFF };
    if (auto refusal = RefusePeer(peer, opRaw); refusal.has_value())
        co_return *std::move(refusal);
    co_return Cc::RefuseWithoutCounter(NotThroughAnswer,
                                       "SUBSCRIBE is answered as a stream, which this caller cannot carry");
}

std::optional<std::vector<std::byte>> LiveStatsResponder::RefusePeer(std::string_view peer, std::uint8_t /*opRaw*/) const
{
    if (_membership.Classify(peer) == Distributed::Membership::Member)
        return std::nullopt;
    return Cc::Refuse(_metrics, RefusedNotAMember, "this node streams its live stats to fleet members only");
}

std::vector<std::byte> LiveStatsResponder::RefusalReply(Wire::PrePayloadDecision decision,
                                                        std::uint8_t /*opRaw*/,
                                                        std::string_view detail) const
{
    return AnswerRefusal(_metrics, Wire::ErrorCodeFor(decision), PrePayloadPolicy(decision), detail);
}

std::vector<std::byte> LiveStatsResponder::EndpointRefusalReply(EndpointRefusal refusal,
                                                                std::uint8_t /*opRaw*/,
                                                                std::string_view detail) const
{
    auto const& row = EndpointRefusals.at(static_cast<std::size_t>(refusal));
    return AnswerRefusal(_metrics, ErrorCodeFor(refusal), row.policy, detail);
}

IFrameStream* LiveStatsResponder::StreamFor(std::uint8_t opRaw) noexcept
{
    return opRaw == static_cast<std::uint8_t>(Wire::Op::Subscribe) ? this : nullptr;
}

std::optional<LiveStatsResponder::TickView> LiveStatsResponder::Observe(Wire::LiveSubject subject,
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

std::optional<std::vector<std::byte>> LiveStatsResponder::RefuseSubscription(Wire::SubscribeRequest const& request,
                                                                             std::string_view peer) const
{
    if (!LiveSubjectServingTable.at(static_cast<std::size_t>(request.subject)).fleetGates)
        return std::nullopt;

    auto const leadership = _sources.Leadership();
    if (!leadership.has_value())
        return Cc::RefuseWithoutCounter(
            FleetNotServedHere,
            "this node runs no scheduler, so it has no fleet to stream; subscribe at the fleet's scheduler");

    // The credential BEFORE leadership, so a caller without it is not told where the leader is.
    auto const admitted = _dashboard.Required() ? _dashboard.Matches(request.dashboardToken) : IsLoopbackHost(peer);
    if (!admitted)
        return Cc::Refuse(_metrics,
                          RefusedUnauthenticated,
                          _dashboard.Required() ? "the fleet streams to a caller presenting the dashboard credential"
                                                : "with no --dashboard-token-file the fleet streams to this machine only");

    if (!leadership->leads)
        // The message IS the leader's endpoint, or empty during an election: a client parses it.
        return Cc::RefuseWithoutCounter(NotLeaderAtSubscribe, leadership->leaderEndpoint);
    return std::nullopt;
}

std::optional<std::vector<std::byte>> LiveStatsResponder::EndOfStream(Wire::LiveSubject subject,
                                                                      std::string_view peer,
                                                                      IPushSink const& sink) const
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
            // endpoint serves what it sent.
            _metrics.Increment(IMetricsSink::Counter::LiveSubscriptionsEndedByClient);
            return OrderlyEnd();
        case PeerActivity::Quiet:
            break;
    }

    // Re-asked every tick of the bound oracle, which is the whole defence against removal failing
    // open: see the file's own note.
    if (_membership.Classify(peer) != Distributed::Membership::Member)
        return Cc::Refuse(_metrics, Revoked, "this peer is no longer a fleet member");

    if (LiveSubjectServingTable.at(static_cast<std::size_t>(subject)).fleetGates)
        if (auto const leadership = _sources.Leadership(); !leadership.has_value() || !leadership->leads)
            return Cc::Refuse(
                _metrics, EndedNotLeader, leadership.has_value() ? leadership->leaderEndpoint : std::string {});

    return std::nullopt;
}

Task<std::vector<std::byte>> LiveStatsResponder::Serve(std::span<std::byte const> frame, std::string peer, IPushSink* sink)
{
    // The authority, as `Answer` is in every sibling: the door's `RefusePeer` is an early-out.
    if (auto refusal = RefusePeer(peer, static_cast<std::uint8_t>(Wire::Op::Subscribe)); refusal.has_value())
        co_return *std::move(refusal);

    auto const request = frame.size() >= Wire::RequestHeaderSize
                             ? Wire::DecodeSubscribeRequest(frame.subspan(Wire::RequestHeaderSize))
                             : std::nullopt;
    if (!request.has_value())
        co_return Cc::Refuse(
            _metrics, RefusedMalformed, "a SUBSCRIBE carries a subject this build serves, a cadence and a dashboard token");
    if (auto refusal = RefuseSubscription(*request, peer); refusal.has_value())
        co_return *std::move(refusal);

    ActiveSubscription const place { _active };
    if (!place.Claimed())
        co_return Cc::Refuse(_metrics,
                             RefusedAtCapacity,
                             std::format("this node already streams to {} watchers", Wire::MaxLiveSubscriptions));

    auto const subject = request->subject;
    auto const floor = Wire::LiveSubjectTable.at(static_cast<std::size_t>(subject)).floor;
    auto const granted = Wire::GrantLiveCadence(subject, request->cadenceMillis);
    auto const hold = std::max(Wire::MinLiveStreamWriteStall, granted * Wire::LiveIdleCadences);
    auto tick = TickOf(_reactor.Clock().Now(), floor);

    // The baseline: this subscriber's event cursor starts at NOW, so it is not replayed events
    // from before it arrived.
    auto const baseline = Observe(subject, tick, std::numeric_limits<std::uint64_t>::max());
    if (!baseline.has_value())
        co_return std::vector<std::byte> {}; // The node is stopping.

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
            .statsLayout = LiveSubjectServingTable.at(static_cast<std::size_t>(subject)).carriesStatsReading
                               ? StatsReadingLayout
                               : std::uint64_t { 0 },
            .endpoint = _sources.AnsweringEndpoint() }));
        outcome != PushOutcome::Delivered)
        co_return failed(outcome);

    LiveCursor cursor { .nextDue = tick,
                        .ticksPerCadence =
                            static_cast<std::uint64_t>((granted + floor - std::chrono::milliseconds { 1 }) / floor) };
    auto eventsFrom = baseline->eventsEnd;

    while (true)
    {
        if (auto end = EndOfStream(subject, peer, *sink); end.has_value())
            co_return *std::move(end);

        tick = TickOf(_reactor.Clock().Now(), floor);
        auto const view = Observe(subject, tick, eventsFrom);
        if (!view.has_value())
            co_return OrderlyEnd(); // The sources were detached: this node is stopping.
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

        co_await SleepUntil { .reactor = &_reactor, .deadline = NextTickAt(tick, floor) };
    }
}

} // namespace FastCache::Node

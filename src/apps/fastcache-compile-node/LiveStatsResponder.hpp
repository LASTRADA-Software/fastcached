// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "FrameEndpoint.hpp"

#include <FastCache/Async/IReactor.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Distributed/SchedulerService.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>
#include <FastCache/Server/AdminCredential.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Node
{

/// @file LiveStatsResponder.hpp
/// Live stats as a subscription: `Op::Subscribe`, answered by a stream of `Status::Push` frames
/// (#1399).
///
/// ## The shape: every subscriber PULLS, and a render is shared per tick
///
/// A subscription is a coroutine on the node's reactor that wakes on its subject's tick grid --
/// every floor (`CompileCacheWire::LiveSubjectTable`) -- re-gates the peer, and pushes what is
/// owed. What it pushes is read from a per-subject cache keyed on the TICK, so the first
/// subscriber to wake on a tick captures and renders and every other one on that tick shares the
/// bytes: `LiveSnapshotsRendered` rises once per subject per tick however many are watching.
///
/// **There is no queue, and that is what makes a slow subscriber free for everybody else.** A
/// subscriber whose push parks -- a watcher that stopped reading -- simply is not awake for the
/// ticks that pass meanwhile. When its write completes it wakes on the current tick, is told how
/// many cadences it missed (`PushKind::Gap`) and gets the current snapshot. Nothing is held for it
/// and nothing waits on it; memory is one cached body per subject, never one per subscriber. A
/// push that stays parked past its hold ends the connection (`IPushSink::Push`).
///
/// ## Events are diffs, not hooks
///
/// Each capture yields a `LiveEventProbe`, and consecutive probes are compared
/// (`DiffLiveEvents`). A hook at every mutation path is a silent missing event the day a path is
/// added without it -- two ends, one forgotten -- while a diff over what the tick already reads
/// cannot miss one. The cost is latency bounded by the subject's floor.
///
/// ## Re-gating is the tick's, and removal is the direction it exists for
///
/// Every tick asks the bound membership oracle again, so a host removed by a reload or by a
/// replicated `ClusterForget` loses its stream within one floor. Admission at subscribe alone
/// would fail OPEN on removal -- the stream would outlive the decision -- and nothing would
/// report it. A fleet stream also ends the tick its node stops leading, naming the new leader.

/// One fact an event is diffed from: a key that decides whether it changed, and words for a
/// person.
struct LiveFact
{
    std::string key;    ///< Compared; a different key is a change.
    std::string detail; ///< What the event says, for a person to read.

    /// Facts are the same when their keys are, whatever the words say.
    [[nodiscard]] bool operator==(LiveFact const& other) const noexcept
    {
        return key == other.key;
    }
};

/// What a subject's events are diffed from, as one capture saw it.
///
/// Absent is a state of its own on every scalar, for the metrics rule's reason: a node that runs
/// no enrollment window has none to report, and appearing or disappearing is itself a change.
struct LiveEventProbe
{
    std::vector<LiveFact> members;         ///< The cluster's members, sorted by key.
    std::vector<LiveFact> workers;         ///< The scheduler's registry, sorted by key.
    std::optional<LiveFact> leadership {}; ///< Who leads, as this node sees it.
    std::optional<LiveFact> survey {};     ///< The toolchain survey's state.
    std::optional<LiveFact> enrollment {}; ///< The enrollment window's state.
};

/// The events between two captures of one subject.
///
/// Pure, so every kind is a unit test over two literals. Set rows report additions before
/// removals, and scalar rows in table order.
/// @param before The previous capture's probe.
/// @param after This capture's probe.
/// @return What changed, in a stable order.
[[nodiscard]] std::vector<CompileCacheWire::LiveEventFields> DiffLiveEvents(LiveEventProbe const& before,
                                                                            LiveEventProbe const& after);

/// Where one subscriber is on its subject's tick grid.
struct LiveCursor
{
    std::uint64_t nextDue { 0 };         ///< The first tick the next snapshot is owed at.
    std::uint64_t ticksPerCadence { 1 }; ///< The granted cadence, in the subject's ticks. Never zero.
};

/// What one subscriber owes on one tick.
struct LiveStep
{
    bool snapshot { false };                               ///< Push this tick's snapshot.
    std::optional<CompileCacheWire::LiveGapFields> gap {}; ///< Say first how many were missed.
};

/// Decide what a subscriber owes on @p tick, and move its cursor.
///
/// **A gap is whole cadences a subscriber was not awake for**, which is how a parked push shows
/// up: the subscriber wakes on a tick past its due one. Late by less than a cadence is not a gap
/// -- that is wake-up jitter, and calling it one would draw a hole in a healthy panel.
/// @param cursor The subscriber's cursor; advanced when a snapshot is owed.
/// @param tick The current tick.
/// @param forced Whether an event this tick owes a snapshot whatever the cadence says.
/// @return What to push.
[[nodiscard]] LiveStep DecideLiveStep(LiveCursor& cursor, std::uint64_t tick, bool forced) noexcept;

/// One capture of a subject: the body a snapshot carries, and what events are diffed from.
struct LiveCapture
{
    std::vector<std::byte> body; ///< The subject's own grammar; see `LiveSnapshotView`.
    LiveEventProbe probe;        ///< What this capture says for `DiffLiveEvents`.
};

/// Who leads, as the fleet gate needs it.
struct LiveLeadership
{
    bool leads { false };       ///< This node leads the fleet.
    std::string leaderEndpoint; ///< Where the leader answers, or empty during an election.
};

/// What a live-stats stream reads.
///
/// A seam because every answer is ambient -- the counters, the storage, the registry, the cluster
/// -- and because two of them are built after the node's `0xFC` surface is already listening
/// (`LiveStatsSourceSlot` carries why).
class ILiveStatsSources
{
  public:
    ILiveStatsSources() = default;
    ILiveStatsSources(ILiveStatsSources const&) = delete;
    ILiveStatsSources(ILiveStatsSources&&) = delete;
    ILiveStatsSources& operator=(ILiveStatsSources const&) = delete;
    ILiveStatsSources& operator=(ILiveStatsSources&&) = delete;
    virtual ~ILiveStatsSources() = default;

    /// Capture @p subject now.
    /// @param subject What to capture.
    /// @return The capture, or nullopt when this node cannot answer it any more -- it is stopping.
    [[nodiscard]] virtual std::optional<LiveCapture> Capture(CompileCacheWire::LiveSubject subject) const = 0;

    /// @return Who leads, or nullopt when this node runs no scheduler and so serves no fleet.
    [[nodiscard]] virtual std::optional<LiveLeadership> Leadership() const = 0;

    /// @return The address this node's `0xFC` surface answers on, for the dashboard's source line.
    [[nodiscard]] virtual std::string AnsweringEndpoint() const = 0;
};

/// The sources a live stream reads, attached once they exist and detached before they go.
///
/// **Why a slot rather than a constructor argument.** The node's `0xFC` surface is started before
/// consensus, because consensus is told the surface's bound endpoint -- and the scrape provider,
/// the fleet sources and the sampler a stream reads are built after consensus, since they read
/// it. So the responder is built first against this slot, the sources are attached before the
/// reactor starts serving, and they are **detached before the first of them is destroyed**.
/// Destruction order cannot express that last half -- the surface outlives what it reads -- which
/// is the `SetHistorySink(nullptr)` shape in `main.cpp`, one step over.
///
/// **The detach is the attachment's destructor**, so it is a fact about declaration order rather
/// than a line at one return path: the attachment is declared after the sources it names, and so
/// is destroyed before them on every way out of the scope.
///
/// A detached slot answers every question with nothing, so a stream still open ends at its next
/// tick rather than reading freed memory. The lock is shared by every reader and taken
/// exclusively only to attach and to detach, so a detach returns only once no capture is running.
class LiveStatsSourceSlot final: public ILiveStatsSources
{
  public:
    /// Holds sources attached; detaches them when destroyed.
    class Attachment
    {
      public:
        /// @param slot The slot to detach from.
        explicit Attachment(LiveStatsSourceSlot& slot) noexcept:
            _slot { &slot }
        {
        }

        ~Attachment()
        {
            _slot->Detach();
        }

        Attachment(Attachment const&) = delete;
        Attachment(Attachment&&) = delete;
        Attachment& operator=(Attachment const&) = delete;
        Attachment& operator=(Attachment&&) = delete;

      private:
        LiveStatsSourceSlot* _slot;
    };

    LiveStatsSourceSlot() = default;

    /// Start answering from @p sources, until the returned attachment is destroyed.
    /// @param sources What to read; must outlive the attachment.
    /// @return The attachment. Declare it after @p sources.
    [[nodiscard]] Attachment Attach(ILiveStatsSources const& sources);

    /// Stop answering, and wait for any capture in progress to finish.
    void Detach();

    /// @copydoc ILiveStatsSources::Capture
    [[nodiscard]] std::optional<LiveCapture> Capture(CompileCacheWire::LiveSubject subject) const override;

    /// @copydoc ILiveStatsSources::Leadership
    [[nodiscard]] std::optional<LiveLeadership> Leadership() const override;

    /// @copydoc ILiveStatsSources::AnsweringEndpoint
    [[nodiscard]] std::string AnsweringEndpoint() const override;

  private:
    mutable std::shared_mutex _mutex;
    ILiveStatsSources const* _sources { nullptr };
};

/// How many events one subject keeps for subscribers that have not read them yet.
///
/// A subscriber reads every event on the tick it wakes, so only a subscriber parked in a write
/// for longer falls behind -- and it is told about that as a gap anyway. Bounded because nothing
/// else about this component grows with how slow a watcher is.
inline constexpr std::size_t LiveEventBacklog = 64;

/// Answers `Op::Subscribe` with a stream.
///
/// **Gated on membership, as `NodeStatusResponder` is**, for its reasons: what streams is what
/// those verbs report, and an operator watching a fleet is by construction not sitting on every
/// node in it. The fleet subject adds two gates -- the dashboard credential, and leadership --
/// because the fleet map is behind the dashboard credential on `/fleet`, and a follower's registry
/// is a partial picture presented as the whole.
class LiveStatsResponder final: public IFrameResponder, public IFrameStream
{
  public:
    /// @param sources What streams read; must outlive this.
    /// @param membership Who may watch; must outlive this. Bound once, by reference, the way every
    ///        surface binds it -- a test that re-acquired it would pass under exactly the removal
    ///        defect re-gating exists for.
    /// @param dashboard The dashboard credential, or a default one when no token file is named --
    ///        and then the fleet is streamed to loopback peers only.
    /// @param reactor The loop streams sleep on; the node's `0xFC` reactor.
    /// @param metrics Where the stream's counters rise; must outlive this.
    LiveStatsResponder(ILiveStatsSources const& sources,
                       Distributed::IMembershipOracle const& membership,
                       AdminCredential dashboard,
                       IReactor& reactor,
                       IMetricsSink& metrics) noexcept;

    /// @copydoc IFrameResponder::Answer
    ///
    /// Reached only by a caller that does not stream, since the endpoint asks `StreamFor` first:
    /// such a caller cannot carry a subscription, so it is told the verb is not served that way.
    [[nodiscard]] Task<std::vector<std::byte>> Answer(std::span<std::byte const> frame, std::string peer) override;

    /// @copydoc IFrameResponder::RefusePeer
    [[nodiscard]] std::optional<std::vector<std::byte>> RefusePeer(std::string_view peer, std::uint8_t opRaw) const override;

    /// @copydoc IFrameResponder::AuthRequired
    ///
    /// **No**, for `NodeStatusResponder::AuthRequired`'s first reason: the credential on this
    /// listener is the scheduler's, and a surface must not require a secret it cannot verify. The
    /// fleet's own secret is the dashboard credential, which the request carries.
    [[nodiscard]] bool AuthRequired(std::uint8_t /*opRaw*/) const noexcept override
    {
        return false;
    }

    /// @copydoc IFrameResponder::CheckCredential
    [[nodiscard]] CredentialOutcome CheckCredential(std::span<std::byte const> payload) const override
    {
        return FastCache::CheckCredential(nullptr, payload);
    }

    /// @copydoc IFrameResponder::RefusalReply
    [[nodiscard]] std::vector<std::byte> RefusalReply(CompileCacheWire::PrePayloadDecision decision,
                                                      std::uint8_t opRaw,
                                                      std::string_view detail) const override;

    /// @copydoc IFrameResponder::EndpointRefusalReply
    [[nodiscard]] std::vector<std::byte> EndpointRefusalReply(EndpointRefusal refusal,
                                                              std::uint8_t opRaw,
                                                              std::string_view detail) const override;

    /// @copydoc IFrameResponder::RequestTimeout
    ///
    /// The header window: it covers reading a control-sized request, and the stream that follows
    /// is bounded per push by its hold rather than by a window over the whole subscription.
    [[nodiscard]] std::chrono::milliseconds RequestTimeout(std::uint8_t /*opRaw*/) const noexcept override
    {
        return FrameServer::HeaderTimeout;
    }

    /// @copydoc IFrameResponder::MaxRequestBytes
    [[nodiscard]] std::size_t MaxRequestBytes() const noexcept override
    {
        return CompileCacheWire::MaxControlPayload;
    }

    /// @copydoc IFrameResponder::MaxOpenConnections
    ///
    /// The subscription cap. Not folded by `MergedResponder::Largest`, like the node component's;
    /// the cap that bites is the one this responder enforces itself, with a counted refusal.
    [[nodiscard]] std::size_t MaxOpenConnections() const noexcept override
    {
        return CompileCacheWire::MaxLiveSubscriptions;
    }

    /// @copydoc IFrameResponder::MaxInFlightBytes
    [[nodiscard]] std::size_t MaxInFlightBytes() const noexcept override
    {
        return CompileCacheWire::MaxControlPayload * 16;
    }

    /// @copydoc IFrameResponder::HoldsOwnByteBudget
    ///
    /// **Yes**, and the accounting is the subscription cap: a request is released by the endpoint
    /// once READ, because a stream lasts hours and a budget charged for its whole length would
    /// count dashboards as load. What one subscription holds is bounded by `MaxRequestBytes`, and
    /// how many there are by `MaxLiveSubscriptions`.
    [[nodiscard]] bool HoldsOwnByteBudget(std::uint8_t /*opRaw*/) const noexcept override
    {
        return true;
    }

    /// @copydoc IFrameResponder::PeerWatchCounter
    ///
    /// **Not watched as an answer**: the endpoint arms a stream its own consulted watch, because a
    /// subscriber leaving is the ordinary end of a stream and not a delivery abandoned mid-answer.
    [[nodiscard]] std::optional<IMetricsSink::Counter> PeerWatchCounter(std::uint8_t /*opRaw*/) const noexcept override
    {
        return std::nullopt;
    }

    /// @copydoc IFrameResponder::ProgressInterval
    ///
    /// **Not pulsed**: a stream pushes a snapshot every granted cadence, which is the liveness a
    /// pulse stands in for.
    [[nodiscard]] std::optional<std::chrono::milliseconds> ProgressInterval(std::uint8_t /*opRaw*/) const noexcept override
    {
        return std::nullopt;
    }

    /// @copydoc IFrameResponder::StreamFor
    [[nodiscard]] IFrameStream* StreamFor(std::uint8_t opRaw) noexcept override;

    /// @copydoc IFrameStream::Serve
    [[nodiscard]] Task<std::vector<std::byte>> Serve(std::span<std::byte const> frame,
                                                     std::string peer,
                                                     IPushSink* sink) override;

    /// @return How many subscriptions are streaming right now. For tests.
    [[nodiscard]] std::size_t ActiveSubscriptions() const noexcept
    {
        return _active.load(std::memory_order_acquire);
    }

  private:
    /// One subject's shared state: the capture cached for a tick, and the events since.
    struct SubjectState
    {
        std::optional<std::uint64_t> tick {};                    ///< The tick `body` was captured on.
        std::shared_ptr<std::vector<std::byte> const> body {};   ///< Shared by every subscriber on that tick.
        std::optional<LiveEventProbe> probe {};                  ///< What the next capture is diffed against.
        std::deque<CompileCacheWire::LiveEventFields> events {}; ///< The newest `LiveEventBacklog`.
        std::uint64_t eventsEnd { 0 };                           ///< Sequence number one past the newest event.
    };

    /// What one subscriber reads on one tick.
    struct TickView
    {
        std::shared_ptr<std::vector<std::byte> const> body;    ///< This tick's snapshot body.
        std::vector<CompileCacheWire::LiveEventFields> events; ///< The events it has not read yet.
        std::uint64_t eventsEnd { 0 };                         ///< Where its event cursor moves to.
    };

    /// Capture @p subject for @p tick once, and hand every subscriber on that tick the same bytes.
    /// @param subject Which subject.
    /// @param tick The current tick.
    /// @param eventsFrom The subscriber's event cursor.
    /// @return The tick's view, or nullopt when the sources are detached.
    [[nodiscard]] std::optional<TickView> Observe(CompileCacheWire::LiveSubject subject,
                                                  std::uint64_t tick,
                                                  std::uint64_t eventsFrom);

    /// The refusal a subscription is answered with before it streams, or nullopt to stream.
    /// @param request The decoded request.
    /// @param peer The peer's host.
    /// @return The encoded refusal, or nullopt.
    [[nodiscard]] std::optional<std::vector<std::byte>> RefuseSubscription(CompileCacheWire::SubscribeRequest const& request,
                                                                           std::string_view peer) const;

    /// The terminal reply a running stream ends with on this tick, or nullopt to go on.
    /// @param subject Which subject.
    /// @param peer The peer's host.
    /// @param sink What the endpoint saw.
    /// @return The reply -- empty to close without one -- or nullopt.
    [[nodiscard]] std::optional<std::vector<std::byte>> EndOfStream(CompileCacheWire::LiveSubject subject,
                                                                    std::string_view peer,
                                                                    IPushSink const& sink) const;

    ILiveStatsSources const& _sources;
    Distributed::IMembershipOracle const& _membership;
    AdminCredential _dashboard;
    IReactor& _reactor;
    IMetricsSink& _metrics;

    /// Subscriptions streaming right now, against `MaxLiveSubscriptions`.
    std::atomic<std::size_t> _active { 0 };

    /// Guards `_subjects`. Every stream runs on one reactor thread, so it is uncontended; it is
    /// here so that fact is not the only thing keeping two captures of one tick apart.
    std::mutex _mutex;

    /// One per `CompileCacheWire::LiveSubjectTable` row, in its order.
    std::array<SubjectState, CompileCacheWire::LiveSubjectTable.size()> _subjects {};
};

} // namespace FastCache::Node

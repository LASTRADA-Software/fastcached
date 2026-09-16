// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/IReactor.hpp>
#include <FastCache/Async/Task.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Metrics/StatsReading.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache
{

/// @file LiveStream.hpp
/// Live stats as a subscription: `Op::Subscribe`, answered by a stream of `Status::Push` frames
/// (#1399). The loop both surfaces serve -- a compile node for every subject, the daemon for the
/// cache -- and the seams each of them supplies: what is captured, who is admitted, and how a
/// frame reaches the socket.
///
/// ## The shape: every subscriber PULLS, and a render is shared per tick
///
/// A subscription is a coroutine on its connection's reactor that wakes on its subject's tick
/// grid -- every floor (`CompileCacheWire::LiveSubjectTable`) -- re-gates the peer, and pushes
/// what is owed. What it pushes is read from a per-subject cache keyed on the TICK, so the first
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
/// Every tick asks the surface's gate again (`ILiveGate::Recheck`), so a peer the surface stops
/// admitting loses its stream within one floor. Admission at subscribe alone would fail OPEN on
/// removal -- the stream would outlive the decision -- and nothing would report it.

/// How one push left, as the writing surface observed it.
///
/// **A PRIVATE enum: nothing transmits it and nothing stores it, so it states no ordinals.**
///
/// Three answers rather than a bool, because the stream files two of them under different
/// counters: a push the surface gave up on is a subscriber that stopped READING, and a push the
/// transport lost is a subscriber that LEFT, or the surface stopping. A bool would have to be read
/// as "delivered, or else something", which is the shape this tree keeps having to undo.
enum class PushOutcome : std::uint8_t
{
    Delivered, ///< Every byte went out.
    Stalled,   ///< The push stayed parked past its hold, and the surface closed the connection.
    Lost,      ///< The write failed otherwise: the peer reset, or the surface is stopping.
};

/// What a subscriber has done since its stream began, as the surface's read watch saw it.
///
/// **A PRIVATE enum**, for `PushOutcome`'s reason.
enum class PeerActivity : std::uint8_t
{
    Quiet,    ///< Nothing, which is what a subscriber does.
    Departed, ///< EOF: the peer said goodbye, and nobody is left to write to.
    Reset,    ///< The peer reset the connection. Counted apart from `Departed`, as the compile surface counts them.
    Sent,     ///< Bytes arrived: the peer wants this connection for its next request.
};

/// The surface's half of a stream: the one writer while it lasts, and what the read watch saw.
///
/// **The stream never touches the socket**, which is what keeps a surface's exactly-one-writer
/// property structural: the surface writes every push, bounds it, and watches the read side, and
/// the stream decides only WHAT to push and WHEN to stop. A stream holding the socket would be a
/// second writer that has to remember all three.
class IPushSink
{
  public:
    IPushSink() = default;
    IPushSink(IPushSink const&) = delete;
    IPushSink(IPushSink&&) = delete;
    IPushSink& operator=(IPushSink const&) = delete;
    IPushSink& operator=(IPushSink&&) = delete;
    virtual ~IPushSink() = default;

    /// Write one frame, bounded.
    ///
    /// **The hold is the bound on a PARKED write.** A subscriber that stops reading fills its
    /// receive window, the write parks, and nothing but a close retrieves a parked write -- so the
    /// surface closes it past @p hold, the write resumes with a failure, and the answer is `Stalled`.
    /// @param frame A whole frame; sent in one write, so a parked one is never spliced into.
    /// @param hold How long the write may stay parked before the connection is ended.
    /// @return How it left.
    [[nodiscard]] virtual Task<PushOutcome> Push(std::vector<std::byte> frame, std::chrono::milliseconds hold) = 0;

    /// @return What the peer has done since the stream began. Never consumes a byte.
    [[nodiscard]] virtual PeerActivity Activity() const noexcept = 0;

    /// @return True once the surface is shutting down, which ends every stream.
    [[nodiscard]] virtual bool Stopping() const noexcept = 0;
};

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

/// Whether a subject's snapshot body starts with an `EncodeStatsReading`.
///
/// So `Subscribed` names the layout a client must decode it in: the two stats subjects do, and
/// the fleet's body is `RenderFleetText`'s document.
/// @param subject Which subject.
/// @return True for `Cache` and `Node`.
[[nodiscard]] bool CarriesStatsReading(CompileCacheWire::LiveSubject subject) noexcept;

/// One capture of a subject: the body a snapshot carries, and what events are diffed from.
struct LiveCapture
{
    std::vector<std::byte> body; ///< The subject's own grammar; see `LiveSnapshotView`.
    LiveEventProbe probe;        ///< What this capture says for `DiffLiveEvents`.
};

/// Capture the cache subject: one `EncodeStatsReading` of the reading `/metrics` renders.
///
/// One function for both surfaces that serve the subject, so a daemon's cache panel and a node's
/// cannot be two encodings of one reading.
/// @param metrics The counters.
/// @param snapshot The snapshot beside them.
/// @return The capture; it carries no events.
/// @p surfaces defaults to `EverySurface`, so the DAEMON -- which serves every surface a row is
/// attributed to -- is unchanged. A compile node passes its own, and passes it explicitly:
/// this is the capture the `live-stats cache` panel reads, and the one that reported
/// `connections_accepted 0` on a binary with no writer for that counter (#1484).
/// @param metrics The counter sink.
/// @param snapshot What the provider stated for this call.
/// @param surfaces The surfaces this process serves.
/// @return The capture.
[[nodiscard]] LiveCapture CaptureCacheSubject(IMetricsSink const& metrics,
                                              MetricsSnapshot const& snapshot,
                                              std::span<MetricsSurface const> surfaces = EverySurface);

/// Who leads, as a fleet gate needs it.
struct LiveLeadership
{
    bool leads { false };       ///< This node leads the fleet.
    std::string leaderEndpoint; ///< Where the leader answers, or empty during an election.
};

/// Why a fleet read was answered with no document.
///
/// **A PRIVATE enum**, for `PushOutcome`'s reason: nothing transmits it, and each surface maps it
/// to a wire code of its own.
enum class FleetTextRefusal : std::uint8_t
{
    /// This process has no fleet to read: it runs no scheduler, or it is stopping and its sources
    /// are detached.
    NoFleet,
    /// A section or a range named nothing this build serves.
    UnknownSelector,
};

/// A fleet read that produced no document, and the words that say why.
struct FleetTextDeclined
{
    FleetTextRefusal refusal { FleetTextRefusal::NoFleet }; ///< Why.
    std::string detail {};                                  ///< For a person; for `UnknownSelector`, what this build serves.
};

/// The fleet document for one selection, and whose view of the fleet it is.
struct FleetTextDocument
{
    /// Whether the node that rendered it leads. Read from the snapshot the body was rendered from,
    /// so a document and the leadership it is judged by cannot straddle an election.
    bool leads { false };
    std::string leaderEndpoint {}; ///< Where the leader answers, or empty during an election.
    std::string body {};           ///< `RenderFleetText`'s document.
};

/// What a live-stats stream reads.
///
/// A seam because every answer is ambient -- the counters, the storage, the registry, the
/// cluster -- and because on a node two of them are built after the `0xFC` surface is already
/// listening (`LiveStatsSourceSlot` carries why).
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
    /// @param subject What to capture; one the gate admitted.
    /// @return The capture, or nullopt when this process cannot answer any more -- it is stopping.
    [[nodiscard]] virtual std::optional<LiveCapture> Capture(CompileCacheWire::LiveSubject subject) const = 0;

    /// @return Who leads, or nullopt when this process runs no scheduler and so serves no fleet.
    [[nodiscard]] virtual std::optional<LiveLeadership> Leadership() const = 0;

    /// @return The address this process's `0xFC` surface answers on, for the dashboard's source line.
    [[nodiscard]] virtual std::string AnsweringEndpoint() const = 0;

    /// Render the fleet document once, for one selection (#1391).
    ///
    /// On a compile node this is `Node::AnswerFleetText`, the function `/fleet.txt` answers from,
    /// so the `fleet-text` verb and the route have one renderer by construction. Here rather than
    /// on a seam of its own because its sources are the fleet's, which a node builds after its
    /// `0xFC` surface is listening -- the reason this interface has a slot.
    /// @param section A section key, or empty for every whole-document section.
    /// @param range A range key, or empty for the default day.
    /// @return The document, or why there is none.
    [[nodiscard]] virtual std::expected<FleetTextDocument, FleetTextDeclined> FleetText(std::string_view section,
                                                                                        std::string_view range) const = 0;
};

/// The sources a live stream reads, attached once they exist and detached before they go.
///
/// **Why a slot rather than a constructor argument.** A node's `0xFC` surface is started before
/// consensus, because consensus is told the surface's bound endpoint -- and the scrape provider,
/// the fleet sources and the sampler a stream reads are built after consensus, since they read
/// it. So the stream is built first against this slot, the sources are attached before the
/// reactor starts serving, and they are **detached before the first of them is destroyed**.
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

    /// @copydoc ILiveStatsSources::FleetText
    [[nodiscard]] std::expected<FleetTextDocument, FleetTextDeclined> FleetText(std::string_view section,
                                                                                std::string_view range) const override;

  private:
    mutable std::shared_mutex _mutex;
    ILiveStatsSources const* _sources { nullptr };
};

/// The sources a process that serves only the cache subject reads: the daemon's.
///
/// Leads nothing and captures nothing but the cache subject; a gate refuses the other two before
/// a capture is ever asked for, so a capture of them answers nothing.
class CacheLiveStatsSources final: public ILiveStatsSources
{
  public:
    /// @param metrics The counters `/metrics` renders; must outlive this.
    /// @param snapshot The snapshot `/metrics` renders beside them.
    /// @param endpoint Where this process's `0xFC` port answers.
    CacheLiveStatsSources(IMetricsSink const& metrics,
                          std::function<MetricsSnapshot()> snapshot,
                          std::string endpoint) noexcept;

    /// @copydoc ILiveStatsSources::Capture
    [[nodiscard]] std::optional<LiveCapture> Capture(CompileCacheWire::LiveSubject subject) const override;

    /// @copydoc ILiveStatsSources::Leadership
    [[nodiscard]] std::optional<LiveLeadership> Leadership() const override
    {
        return std::nullopt;
    }

    /// @copydoc ILiveStatsSources::AnsweringEndpoint
    [[nodiscard]] std::string AnsweringEndpoint() const override
    {
        return _endpoint;
    }

    /// @copydoc ILiveStatsSources::FleetText
    ///
    /// No fleet. Unreachable in the daemon, which refuses the verb by name (`RelocatedVerbs`)
    /// before any source is asked; answered rather than left to a caller to assume.
    [[nodiscard]] std::expected<FleetTextDocument, FleetTextDeclined> FleetText(std::string_view /*section*/,
                                                                                std::string_view /*range*/) const override
    {
        return std::unexpected(FleetTextDeclined { .refusal = FleetTextRefusal::NoFleet,
                                                   .detail = "this process is a cache and serves no fleet" });
    }

  private:
    IMetricsSink const& _metrics;
    std::function<MetricsSnapshot()> _snapshot;
    std::string _endpoint;
};

/// Who a surface streams to, asked before a stream begins and again on every tick.
///
/// A seam because the answer is the SURFACE's and the two surfaces differ: a compile node gates on
/// membership, the dashboard credential and leadership, while the daemon gates on the credential
/// its own policy requires and serves one subject. Every refusal is encoded -- and counted -- by
/// the gate, which owns the rows.
class ILiveGate
{
  public:
    ILiveGate() = default;
    ILiveGate(ILiveGate const&) = delete;
    ILiveGate(ILiveGate&&) = delete;
    ILiveGate& operator=(ILiveGate const&) = delete;
    ILiveGate& operator=(ILiveGate&&) = delete;
    virtual ~ILiveGate() = default;

    /// May this peer subscribe at all, before its request is read for what it asks?
    /// @param peer The peer's host.
    /// @return The encoded refusal, or nullopt to go on.
    [[nodiscard]] virtual std::optional<std::vector<std::byte>> RefuseWatcher(std::string_view peer) const = 0;

    /// May this peer have what it asked for?
    /// @param request The decoded request.
    /// @param peer The peer's host.
    /// @return The encoded refusal, or nullopt to stream.
    [[nodiscard]] virtual std::optional<std::vector<std::byte>> Admit(CompileCacheWire::SubscribeRequest const& request,
                                                                      std::string_view peer) const = 0;

    /// Is a running stream still admitted, on this tick?
    /// @param subject What it streams.
    /// @param peer The peer's host.
    /// @return The terminal reply that ends it, or nullopt to go on.
    [[nodiscard]] virtual std::optional<std::vector<std::byte>> Recheck(CompileCacheWire::LiveSubject subject,
                                                                        std::string_view peer) const = 0;
};

/// How many events one subject keeps for subscribers that have not read them yet.
///
/// A subscriber reads every event on the tick it wakes, so only a subscriber parked in a write
/// for longer falls behind -- and it is told about that as a gap anyway. Bounded because nothing
/// else about this component grows with how slow a watcher is.
inline constexpr std::size_t LiveEventBacklog = 64;

/// How often a stream sleeping until its next tick looks up to see whether its surface is stopping.
///
/// A tick is up to a second away, and a node's reactor stops once its adopted loops end -- the
/// sweeper among them, which may observe the stop at once. A stream asleep for the whole tick
/// would then still be parked when the reactor stops, and the stop would wait out its drain
/// ceiling for a stream that had nothing left to do. A tenth of a second keeps the look cheap: a
/// full node of subscribers is a few hundred wake-ups a second of nothing.
inline constexpr std::chrono::milliseconds LiveStopCheck { 100 };

/// Serves subscriptions: the per-subject tick cache, the cap, and the loop every stream runs.
///
/// One per process, shared by every connection on every reactor, and a stream sleeps on the reactor
/// its own connection runs on.
///
/// **No reactor waits on another reactor's capture.** A capture reads the storage stats, which takes
/// every shard lock in turn, so holding one process-wide lock across it would stall every other
/// reactor with a subscriber due on that tick for as long as the slowest shard writer held its lock.
/// Instead a subscriber at a stale tick CLAIMS the capture with a compare-exchange and takes it on
/// its own reactor -- paying the shard-lock waits one command pays -- and publishes the result as an
/// immutable `LiveView`. A subscriber that loses the claim does not wait for it: it looks again one
/// `LiveStopCheck` later and reads what the winner published. The lock that remains guards a
/// pointer copy and nothing else. Nothing hops threads, so no stream's frame is ever resumed on a
/// reactor other than its own, whose teardown order its surface already owns.
class LiveStream
{
  public:
    /// @param sources What streams read; must outlive this.
    /// @param metrics Where the stream's counters rise; must outlive this.
    LiveStream(ILiveStatsSources const& sources, IMetricsSink& metrics) noexcept;

    LiveStream(LiveStream const&) = delete;
    LiveStream(LiveStream&&) = delete;
    LiveStream& operator=(LiveStream const&) = delete;
    LiveStream& operator=(LiveStream&&) = delete;
    ~LiveStream() = default;

    /// Serve one subscription until it ends.
    ///
    /// Parameters are pointers because a coroutine parameter must not be a reference; each must
    /// outlive the returned task.
    /// @param frame The whole request, header included.
    /// @param peer The peer's host.
    /// @param sink Where every push goes, and what the surface's read watch saw.
    /// @param gate Who is admitted, at subscribe and on every tick.
    /// @param reactor Where this stream sleeps between ticks: its connection's reactor.
    /// @return The terminal reply, or empty to close without one.
    [[nodiscard]] Task<std::vector<std::byte>> Serve(
        std::span<std::byte const> frame, std::string peer, IPushSink* sink, ILiveGate const* gate, IReactor* reactor);

    /// @return How many subscriptions are streaming right now.
    [[nodiscard]] std::size_t ActiveSubscriptions() const noexcept
    {
        return _active->load(std::memory_order_acquire);
    }

  private:
    /// One capture of one subject, published whole and never written after it is shared.
    struct LiveView
    {
        std::uint64_t tick { 0 };                                 ///< The tick it was captured on.
        std::vector<std::byte> body {};                           ///< The snapshot body every subscriber sends.
        LiveEventProbe probe {};                                  ///< What the next capture is diffed against.
        std::vector<CompileCacheWire::LiveEventFields> events {}; ///< The newest `LiveEventBacklog`, oldest first.
        std::uint64_t eventsEnd { 0 };                            ///< Sequence number one past the newest event.
    };

    /// One subject's shared state: the newest published view, and whether a capture is being taken.
    struct SubjectState
    {
        std::shared_ptr<LiveView const> published {}; ///< Null before the first capture; copied under `_publish`.
        std::atomic<bool> capturing { false };        ///< Claimed by the one subscriber taking the capture.
    };

    /// What an observation found.
    ///
    /// TRANSMITTED/PERSISTED: no. Private; enumerators may be inserted.
    enum class Seen : std::uint8_t
    {
        Ready,   ///< The tick's view, or the newest one this subscriber has not seen: send it.
        Busy,    ///< Another subscriber is taking a capture, and none is newer than the last seen: look again shortly.
        Stopped, ///< The sources can no longer answer: the process is stopping.
    };

    /// What one subscriber reads on one tick.
    struct Observation
    {
        Seen seen { Seen::Stopped };                              ///< Whether there is a view.
        std::uint64_t tick { 0 };                                 ///< The tick the subscriber looked on.
        std::shared_ptr<LiveView const> view {};                  ///< The view, when `Ready`; carries its capture tick.
        std::vector<CompileCacheWire::LiveEventFields> events {}; ///< The events the subscriber has not read.
        std::uint64_t eventsEnd { 0 };                            ///< Where its event cursor moves to.
    };

    /// Read @p subject's view for @p tick, capturing it when this subscriber wins the claim.
    ///
    /// **A subscriber that loses the claim sends the newest published view it has not seen**, rather
    /// than waiting for the capture in progress. Waiting would be fair only against a capture shorter
    /// than a tick: one that is slower is claimed again the moment it is published, and a subscriber
    /// on another reactor that looks every `LiveStopCheck` then finds it claimed every time, for as
    /// long as the stream runs.
    /// @param subject Which subject.
    /// @param tick The current tick.
    /// @param eventsFrom The subscriber's event cursor.
    /// @param seen The tick of the newest view this subscriber has read, or nullopt before its first.
    /// @return The observation.
    [[nodiscard]] Observation Observe(CompileCacheWire::LiveSubject subject,
                                      std::uint64_t tick,
                                      std::uint64_t eventsFrom,
                                      std::optional<std::uint64_t> seen);

    /// Observe until there is a view or the sources stop, stepping `LiveStopCheck` while another
    /// subscriber holds the claim, so a stream never waits on another reactor's capture.
    ///
    /// Parameters are pointers for `Serve`'s reason.
    /// @param subject Which subject.
    /// @param floor The subject's tick length.
    /// @param eventsFrom The subscriber's event cursor.
    /// @param seen The tick of the newest view this subscriber has read, or nullopt before its first.
    /// @param sink The surface, asked whether it is stopping between looks.
    /// @param reactor Where the stream sleeps between looks.
    /// @return A `Ready` or `Stopped` observation, or `Busy` when the surface began stopping meanwhile.
    [[nodiscard]] Task<Observation> AwaitView(CompileCacheWire::LiveSubject subject,
                                              std::chrono::milliseconds floor,
                                              std::uint64_t eventsFrom,
                                              std::optional<std::uint64_t> seen,
                                              IPushSink const* sink,
                                              IReactor* reactor);

    /// The terminal reply for what the sink saw, or nullopt when the peer is still watching.
    /// @param sink What the surface saw.
    /// @return The reply -- empty to close without one -- or nullopt.
    [[nodiscard]] std::optional<std::vector<std::byte>> EndedBySink(IPushSink const& sink) const;

    ILiveStatsSources const& _sources;
    IMetricsSink& _metrics;

    /// Subscriptions streaming right now, against `MaxLiveSubscriptions`.
    ///
    /// **Shared with every stream's frame, and that is the teardown rule rather than a
    /// convenience.** A stream parked on its reactor's timer when that reactor stops is freed
    /// when the reactor is destroyed -- and a node declares its reactor before, so destroys it
    /// after, the component that owns this. Held by value, the frame's release would decrement
    /// a counter inside a destroyed object; shared, the last reference is the frame's own. What
    /// else a frame's destruction touches it owns.
    std::shared_ptr<std::atomic<std::size_t>> _active { std::make_shared<std::atomic<std::size_t>>(0) };

    /// Guards the `published` pointers and nothing else: never held across a capture.
    std::mutex _publish;

    /// One per `CompileCacheWire::LiveSubjectTable` row, in its order.
    std::array<SubjectState, CompileCacheWire::LiveSubjectTable.size()> _subjects {};
};

} // namespace FastCache

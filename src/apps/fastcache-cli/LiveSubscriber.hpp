// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CliEndpoint.hpp"
#include "NodeClient.hpp"
#include "RespClient.hpp"

#include <FastCache/Metrics/StatsReading.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace FastCache::Cli
{

class NodeExchange;

/// @file LiveSubscriber.hpp
/// A `live-stats` session's one stream: a `SUBSCRIBE` held open, read a frame at a time (#1399).
///
/// **Two halves, split where a socket starts.** `ReadLiveFrame` is pure: it turns one reply into
/// what it says for the subject asked about, so every refusal, every foreign layout and every body
/// that does not decode is a case with no server. `ILiveSubscription` is the blocking half, read on
/// the thread that may block exactly as every other exchange this binary makes is -- and it is the
/// only thing here a session cannot take back, which is why it can be told to leave from anywhere.

/// What one frame of a stream says.
///
/// TRANSMITTED/PERSISTED: no. Private to this process; enumerators may be inserted.
enum class LiveFrameKind : std::uint8_t
{
    Granted,    ///< The stream's first frame: the cadence the server keeps, and where it answers.
    Reading,    ///< The subject at one tick.
    Event,      ///< A discrete change; the same tick's reading follows it.
    Gap,        ///< Readings this client was too slow to read were dropped; the next is the present.
    Ended,      ///< The server ended the stream in order. Subscribing again is the answer.
    Refused,    ///< The server refused the stream, or revoked it, naming why.
    Unreadable, ///< Nothing this build can read: a stream this client must not go on reading.
    Last,
};

/// One frame of a stream, read for the subject it was asked about.
///
/// A flat aggregate rather than a variant, for `DashboardEvent`'s reason: a case is a list of these,
/// and designated initializers are what a person can read. Everything is OWNED -- a reading outlives
/// the bytes it was decoded from, which a view onto the read buffer would not.
struct LiveFrame
{
    LiveFrameKind kind { LiveFrameKind::Unreadable }; ///< What the frame is.

    /// `Granted`: the subject, the cadence kept, the layout the readings come in and the answering endpoint.
    CompileCacheWire::LiveSubscribedFields granted {};

    std::uint64_t tick { 0 }; ///< `Reading`: the server tick it belongs to.

    /// `Reading`, `cache` and `node`: the figures, decoded into the one model `/metrics` renders from.
    std::optional<StatsReading> reading {};

    /// `Reading`, `node`: what the node says about itself on the same tick.
    std::optional<CompileCacheWire::NodeStatusFields> nodeStatus {};

    /// `Reading`, `fleet`: the leader's `RenderFleetText` document, whole.
    std::optional<std::string> document {};

    CompileCacheWire::LiveEventFields event {}; ///< `Event`: what changed.
    std::uint64_t dropped { 0 };                ///< `Gap`: how many readings were dropped.

    /// `Refused`: the server's code; `NotLeader` is an instruction naming where to go.
    std::optional<CompileCacheWire::ErrorCode> code {};

    std::string note {}; ///< `Refused`, `Ended` and `Unreadable`: why, for a person.
};

/// What @p reply says, as a frame of a stream subscribed to @p subject.
///
/// **A layout this build does not read is refused at the grant**, before any reading arrives: a
/// reading laid out by another build decodes to plausible numbers in the wrong places, and the
/// digest in `PushKind::Subscribed` exists so a client can say so by name instead. A reading whose
/// own digest disagrees is refused the same way, since the grant is not what a reading is trusted on.
/// @param subject What the stream was asked for.
/// @param reply One frame, as the exchange read it.
/// @return The frame.
[[nodiscard]] LiveFrame ReadLiveFrame(CompileCacheWire::LiveSubject subject, NodeReply const& reply);

/// One stream to one endpoint, read on a thread that may block.
///
/// **`Leave` is the one member callable from another thread, while `Read` blocks**, and it is how a
/// session ends a stream it cannot take back: it half-closes, the server reads that as the watcher
/// leaving -- counted as a goodbye, never as a reset -- and closes, which is what ends the blocked
/// read. Everything else is called by one thread at a time.
class ILiveSubscription
{
  public:
    ILiveSubscription() = default;
    ILiveSubscription(ILiveSubscription const&) = delete;
    ILiveSubscription(ILiveSubscription&&) = delete;
    ILiveSubscription& operator=(ILiveSubscription const&) = delete;
    ILiveSubscription& operator=(ILiveSubscription&&) = delete;
    virtual ~ILiveSubscription() = default;

    /// Dial @p where, present the credential, and send @p request, replacing any stream open before.
    /// @param where The endpoint: the one this invocation named, or the leader a refusal named.
    /// @param request What to subscribe to.
    /// @return Nothing once the request is sent, or why it could not be.
    [[nodiscard]] virtual std::expected<void, ExchangeError> Open(Endpoint const& where,
                                                                  CompileCacheWire::SubscribeRequest const& request) = 0;

    /// The stream's next frame. Blocks until one arrives, the silence bound passes, or the stream ends.
    /// @return The frame, or why none arrived.
    [[nodiscard]] virtual std::expected<NodeReply, ExchangeError> Read() = 0;

    /// Wait no longer than `LiveIdleBound(cadence)` for each frame from now on.
    ///
    /// Asked once the grant has said what the cadence is: a snapshot is sent every cadence whether or
    /// not anything changed, so that long a silence is a stream that has died, not a quiet one.
    /// @param cadence What the grant said the server keeps.
    virtual void ExpectEvery(std::chrono::milliseconds cadence) = 0;

    /// Leave the stream: half-close, so the server ends it counted as the watcher's goodbye.
    ///
    /// Callable from any thread, including while `Read` blocks on another; a later `Open` is refused.
    virtual void Leave() noexcept = 0;
};

/// The production subscription: one `NodeExchange` at a time, dialled per `Open`.
class NodeSubscription final: public ILiveSubscription
{
  public:
    /// @param timeouts How long a dial may take, and the first frame's read.
    /// @param credential What to present on every dial; nothing when unconfigured.
    NodeSubscription(DialTimeouts timeouts, Credential credential);
    ~NodeSubscription() override;

    NodeSubscription(NodeSubscription const&) = delete;
    NodeSubscription(NodeSubscription&&) = delete;
    NodeSubscription& operator=(NodeSubscription const&) = delete;
    NodeSubscription& operator=(NodeSubscription&&) = delete;

    [[nodiscard]] std::expected<void, ExchangeError> Open(Endpoint const& where,
                                                          CompileCacheWire::SubscribeRequest const& request) override;
    [[nodiscard]] std::expected<NodeReply, ExchangeError> Read() override;
    void ExpectEvery(std::chrono::milliseconds cadence) override;
    void Leave() noexcept override;

  private:
    DialTimeouts _timeouts;
    Credential _credential;

    /// Guards `_exchange` against `Leave`, and `_left`.
    ///
    /// **Not held across a read.** The reading thread is the only one that replaces or destroys the
    /// exchange, and it does so under this lock; `Leave` touches the exchange only under it. So the
    /// reader may use the exchange unlocked, and `Leave` never meets one being destroyed.
    std::mutex _mutex;
    std::unique_ptr<NodeExchange> _exchange;
    bool _left { false };
};

} // namespace FastCache::Cli

// SPDX-License-Identifier: Apache-2.0
//
// The outbound transport. Every case here drives a scripted connector rather
// than a socket: the properties worth pinning are about queueing, dropping and
// shutdown, and a real network would make each of them a race.
#include <FastCache/Async/TestReactor.hpp>
#include <FastCache/Consensus/RaftPeerTransport.hpp>
#include <FastCache/Consensus/RaftWire.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Net/InMemoryTransport.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

using namespace FastCache;
using namespace FastCache::Consensus;
using namespace std::chrono_literals;

namespace
{

/// A socket that records what was written and can be told to fail.
class RecordingSocket final: public ISocket
{
  public:
    /// Shared state, so a test can read it after the transport has closed and
    /// released its socket.
    struct Record
    {
        std::mutex mutex;
        std::condition_variable wake;
        std::vector<std::vector<std::byte>> writes;
        bool failWrites { false };

        /// Park writes instead of answering them, so a test can be inside the
        /// window where a peer has accepted and stopped reading.
        ///
        /// A fake that resolves every write inline cannot show the failure this
        /// whole migration exists to remove: the transport arms no I/O timeout, so
        /// over a blocking socket such a write was uninterruptible and `Stop()`
        /// joined a thread that was not waiting on anything. Only a parked write
        /// can demonstrate that closing the socket is what completes it.
        bool parkWrites { false };
    };

    /// @param record Where to append; must outlive the socket.
    explicit RecordingSocket(std::shared_ptr<Record> record) noexcept:
        _record { std::move(record) }
    {
    }

    [[nodiscard]] IoAwaitable Read(std::span<std::byte> buffer) override
    {
        // Never resolves with data; the transport under test only writes.
        return IoAwaitable { IoResult { buffer.empty() ? std::size_t { 0 } : std::size_t { 0 } } };
    }

    [[nodiscard]] IoAwaitable Write(std::span<std::byte const> buffer) override
    {
        std::scoped_lock const guard { _record->mutex };
        if (_record->failWrites)
            return IoAwaitable { std::unexpected {
                NetError { .code = NetErrorCode::ConnReset, .systemCode = 0, .context = "scripted write failure" } } };

        if (_record->parkWrites)
        {
            // Deferred, and stashed so `Close()` can complete it -- mirroring
            // `EpollSocket`, where closing is exactly what resolves a parked
            // operation.
            IoAwaitable parked;
            parked.SetSuspendCallback(
                [](IoAwaitable* self, std::coroutine_handle<>) {
                    auto* const socket = static_cast<RecordingSocket*>(self->CallbackState());
                    socket->_parkedWrite = self;
                },
                this);
            return parked;
        }

        _record->writes.emplace_back(buffer.begin(), buffer.end());
        _record->wake.notify_all();
        return IoAwaitable { IoResult { buffer.size() } };
    }

    [[nodiscard]] IoAwaitable WriteVectored(std::span<std::span<std::byte const> const> segments,
                                            std::shared_ptr<void const> keepAlive = {}) override
    {
        std::ignore = keepAlive;
        std::size_t total = 0;
        for (auto const& segment: segments)
            total += segment.size();
        return IoAwaitable { IoResult { total } };
    }

    void Close() noexcept override
    {
        _closed = true;
        // Completing the parked operation is what `Close` MEANS on a reactor
        // socket, and it is the whole reason a stop can now reach a sender that
        // is mid-write.
        if (auto* const parked = std::exchange(_parkedWrite, nullptr); parked != nullptr)
            parked->Complete(std::unexpected {
                NetError { .code = NetErrorCode::Cancelled, .systemCode = 0, .context = "socket closed" } });
    }

    [[nodiscard]] bool IsClosed() const noexcept override
    {
        return _closed;
    }

    [[nodiscard]] std::string PeerAddress() const override
    {
        return "scripted";
    }

  private:
    std::shared_ptr<Record> _record;
    IoAwaitable* _parkedWrite { nullptr };
    bool _closed { false };
};

/// A connector under the test's control: it can refuse, or hand out sockets
/// that record what the transport wrote.
class ScriptedConnector final: public IConnector
{
  public:
    /// @param record Shared write log handed to every socket produced.
    explicit ScriptedConnector(std::shared_ptr<RecordingSocket::Record> record) noexcept:
        _record { std::move(record) }
    {
    }

    /// Make every subsequent dial fail.
    /// @param refuse Whether to refuse.
    void Refuse(bool refuse) noexcept
    {
        _refuse.store(refuse, std::memory_order_relaxed);
    }

    /// @return How many dials have been attempted.
    [[nodiscard]] std::size_t Attempts() const noexcept
    {
        return _attempts.load(std::memory_order_relaxed);
    }

    /// @copydoc IConnector::Connect
    ///
    /// A coroutine that never suspends, mirroring `BlockingConnector`: the
    /// transport still drives it with `SyncRun`, and a fake that suspended would
    /// make these cases assert something no production connector does.
    [[nodiscard]] Task<SocketResult> Connect(std::string host,
                                             std::uint16_t port,
                                             std::chrono::milliseconds connectTimeout) override
    {
        std::ignore = host;
        std::ignore = port;
        std::ignore = connectTimeout;
        _attempts.fetch_add(1, std::memory_order_relaxed);
        if (_refuse.load(std::memory_order_relaxed))
            co_return std::unexpected { NetError {
                .code = NetErrorCode::ConnRefused, .systemCode = 0, .context = "scripted refusal" } };
        co_return std::make_unique<RecordingSocket>(_record);
    }

  private:
    std::shared_ptr<RecordingSocket::Record> _record;
    std::atomic<bool> _refuse { false };
    std::atomic<std::size_t> _attempts { 0 };
};

/// A vote response, the smallest message that round-trips.
/// @param term Term to carry.
/// @return The message.
[[nodiscard]] RaftMessage Vote(std::uint64_t term)
{
    return RaftMessage { RequestVoteResponse {
        .term = Term { .value = term }, .decision = VoteDecision::Granted, .voterId = "n1" } };
}

/// One peer pointing anywhere; the scripted connector ignores the address.
/// @return The endpoint table.
[[nodiscard]] std::vector<PeerEndpoint> OnePeer()
{
    return { PeerEndpoint { .id = "n2", .host = "unused", .port = 1 } };
}

/// Clock, reactor and transport, wired the way production wires them.
///
/// Every wall-clock wait has left this file. The senders run on a `TestReactor`
/// driven by the test's own thread, so "let the sender make progress" is
/// `Drain()` and "let the backoff elapse" is `Advance()`. What that buys is not
/// only speed: the old file had a case whose comment described a race it had to
/// engineer around, and that race no longer exists.
struct Harness
{
    std::shared_ptr<RecordingSocket::Record> record { std::make_shared<RecordingSocket::Record>() };
    ManualClock clock;
    TestReactor reactor { clock };
    ScriptedConnector connector { record };
    NullLogger logger;
    std::unique_ptr<RaftPeerTransport> transport;

    /// Build the transport over one peer and start its sender.
    /// @param options Timeouts and queue bound.
    void Start(PeerTransportOptions options = {})
    {
        transport = std::make_unique<RaftPeerTransport>(NodeId { "n1" }, OnePeer(), reactor, connector, logger, options);
        transport->Start();
        reactor.Drain();
    }

    /// Ask the senders to finish and let them, without the blocking `Stop()`.
    ///
    /// Tests drive the reactor from their own thread, so they must never call the
    /// waiting form: it waits for coroutines only that thread can advance. That
    /// is what `RequestStop` exists for.
    /// @param wakeBound How far to advance the clock, so a parked backoff wakes.
    void RequestStopAndDrain(std::chrono::milliseconds wakeBound = std::chrono::milliseconds { 50 })
    {
        transport->RequestStop();
        reactor.Drain();
        clock.Advance(wakeBound);
        reactor.Drain();
    }

    /// @return How many frames the peer socket has been handed.
    [[nodiscard]] std::size_t Writes() const
    {
        std::scoped_lock const guard { record->mutex };
        return record->writes.size();
    }

    /// @return The bytes of one recorded frame.
    /// @param index Which frame.
    [[nodiscard]] std::vector<std::byte> Frame(std::size_t index) const
    {
        std::scoped_lock const guard { record->mutex };
        return record->writes.at(index);
    }
};

} // namespace

TEST_CASE("A sent message reaches the peer as a decodable frame", "[consensus][raft][transport]")
{
    Harness harness;
    harness.Start();

    harness.transport->Send("n2", Vote(7));
    // Not yet: `Push` hands the sender's handle to the reactor rather than
    // resuming it, which is what keeps it out of the driver's mutex.
    CHECK(harness.Writes() == 0);

    harness.reactor.Drain();
    REQUIRE(harness.Writes() == 1);

    // Asserted by decoding rather than by byte count: what matters is that the
    // peer can read what arrived, which a length check would not establish.
    auto const frame = harness.Frame(0);
    auto const header = RaftWire::DecodeHeader(frame);
    REQUIRE(header.has_value());
    auto const decoded = RaftWire::DecodeMessage(header.value_or(RaftWire::FrameHeader {}),
                                                 std::span<std::byte const> { frame }.subspan(RaftWire::HeaderSize));
    REQUIRE(decoded.has_value());
    auto const message = decoded.value_or(RaftMessage {});
    REQUIRE(std::holds_alternative<RequestVoteResponse>(message));
    CHECK(std::get<RequestVoteResponse>(message).term == Term { .value = 7 });

    harness.RequestStopAndDrain();
}

TEST_CASE("Send hands the message over without advancing the sender", "[consensus][raft][transport]")
{
    // Sharper than "Send does not block", which a merely fast implementation
    // would also satisfy. `Send` is reached from `RaftDriver::Deliver` while it
    // holds the driver's mutex, so a queue that resumed the sender inline would
    // run the sender's next step under that lock -- and the sender calls back
    // into the transport.
    Harness harness;
    harness.Start();

    for (auto term = 1; term <= 4; ++term)
        harness.transport->Send("n2", Vote(static_cast<std::uint64_t>(term)));

    CHECK(harness.Writes() == 0);
    harness.reactor.Drain();
    CHECK(harness.Writes() == 4);

    harness.RequestStopAndDrain();
}

TEST_CASE("A message for this node itself is not sent anywhere", "[consensus][raft][transport]")
{
    // A configuration listing every member uniformly is the natural thing to
    // write, so the transport tolerates its own id rather than making each caller
    // filter -- but it must not give itself a socket and a sender.
    auto record = std::make_shared<RecordingSocket::Record>();
    ManualClock clock;
    TestReactor reactor { clock };
    ScriptedConnector connector { record };
    NullLogger logger;

    std::vector<PeerEndpoint> peers { PeerEndpoint { .id = "n1", .host = "self", .port = 1 },
                                      PeerEndpoint { .id = "n2", .host = "unused", .port = 2 } };
    RaftPeerTransport transport { "n1", std::move(peers), reactor, connector, logger };
    transport.Start();
    reactor.Drain();

    transport.Send("n1", Vote(1));
    transport.Send("n2", Vote(2));
    reactor.Drain();

    {
        std::scoped_lock const guard { record->mutex };
        // Exactly one write: the peer's. The self-addressed one was refused.
        CHECK(record->writes.size() == 1);
    }

    // Drained before the transport is destroyed. Its destructor calls the
    // waiting `Stop()`, which asserts when called from the thread the senders run
    // on -- and in a test that thread is this one.
    transport.RequestStop();
    reactor.Drain();
    clock.Advance(50ms);
    reactor.Drain();
    REQUIRE(transport.SendersRunning() == 0);
}

TEST_CASE("A message for an unknown peer is dropped and counted", "[consensus][raft][transport]")
{
    // Counted rather than thrown. This is reached from the driver's send loop, and
    // a throw there would take a node down over a configuration gap that costs it
    // one peer.
    Harness harness;
    harness.Start();

    harness.transport->Send("nobody", Vote(1));
    CHECK(harness.transport->DroppedMessages() == 1);

    harness.RequestStopAndDrain();
}

TEST_CASE("A queue for an unreachable peer is bounded", "[consensus][raft][transport]")
{
    // The property that keeps a peer being down from becoming a memory leak: an
    // hour of heartbeats for an unreachable follower must not accumulate.
    constexpr std::size_t Bound = 4;
    Harness harness;
    harness.connector.Refuse(true);
    harness.Start(PeerTransportOptions { .dialTimeout = 10s, .reconnectBackoff = 10s, .maxQueuedPerPeer = Bound });

    for (auto term = 1; term <= 10; ++term)
        harness.transport->Send("n2", Vote(static_cast<std::uint64_t>(term)));

    // Six displaced, and reported: a drop nobody counts is invisible, and Raft
    // recovering from loss means a fleet dropping steadily looks healthy while
    // running slower than it should.
    CHECK(harness.transport->DroppedMessages() == 10 - Bound);
    CHECK(harness.Writes() == 0);

    harness.RequestStopAndDrain();
}

TEST_CASE("Stopping is prompt even while a peer is unreachable", "[consensus][raft][transport]")
{
    // The assertion is on the CLOCK, not on elapsed wall time. A version that
    // merely happened to be fast would pass a wall-clock bound; what has to hold
    // is that teardown does not wait out `reconnectBackoff`, so shutdown stays
    // independent of how unreachable a peer happens to be.
    constexpr auto Backoff = 30s;
    constexpr auto WakeBound = 50ms;

    Harness harness;
    harness.connector.Refuse(true);
    harness.Start(PeerTransportOptions { .dialTimeout = 10s, .reconnectBackoff = Backoff, .stopWakeBound = WakeBound });

    auto const started = harness.clock.Now();
    harness.RequestStopAndDrain(WakeBound);

    CHECK(harness.transport->SendersRunning() == 0);
    // Woken within the bound, not after the backoff.
    CHECK(harness.clock.Now() - started == WakeBound);
    CHECK(harness.reactor.PendingTimers() == 0);
    CHECK(harness.reactor.PendingSubmissions() == 0);
}

TEST_CASE("A refused dial is not retried faster than the backoff", "[consensus][raft][transport]")
{
    // The dial-storm guard. Without it a partitioned node opens connections as
    // fast as it can to a host that is down, which fills a conntrack table and
    // starts breaking unrelated traffic to that host.
    constexpr auto Backoff = 200ms;
    Harness harness;
    harness.connector.Refuse(true);
    harness.Start(PeerTransportOptions { .dialTimeout = 1s, .reconnectBackoff = Backoff, .stopWakeBound = 50ms });

    REQUIRE(harness.connector.Attempts() == 1);

    harness.clock.Advance(Backoff / 2);
    harness.reactor.Drain();
    CHECK(harness.connector.Attempts() == 1);

    harness.clock.Advance(Backoff);
    harness.reactor.Drain();
    CHECK(harness.connector.Attempts() == 2);

    harness.RequestStopAndDrain();
}

TEST_CASE("A connection that drops is also backed off", "[consensus][raft][transport]")
{
    // The regression guard for the shape the thread version had: it redialled
    // immediately after a dropped connection, so a peer that accepts and instantly
    // resets produced a tight connect/write-fail/connect loop with no sleep in it.
    // On a private thread that burned one core; on the shared reactor it starves
    // the election timers, which is the "nine role changes in twelve seconds"
    // failure this repository already has a name for.
    constexpr auto Backoff = 200ms;
    Harness harness;
    harness.Start(PeerTransportOptions { .dialTimeout = 1s, .reconnectBackoff = Backoff, .stopWakeBound = 50ms });
    REQUIRE(harness.connector.Attempts() == 1);

    // A write that fails ends the session, so the sender goes round the loop.
    harness.record->failWrites = true;
    harness.transport->Send("n2", Vote(1));
    harness.reactor.Drain();

    // Still one: the redial waits for the backoff rather than happening at once.
    CHECK(harness.connector.Attempts() == 1);

    harness.clock.Advance(Backoff);
    harness.reactor.Drain();
    CHECK(harness.connector.Attempts() == 2);

    harness.RequestStopAndDrain();
}

TEST_CASE("Stopping completes a write the peer never read", "[consensus][raft][transport]")
{
    // THE case. The transport arms no I/O timeout on its sockets, deliberately --
    // that decides when a peer is declared dead and is its own decision. Over a
    // blocking socket that made a write uninterruptible: a peer that accepted and
    // stopped reading parked the sender in `::send` once the buffer filled, and
    // `Stop()` cleared the outbox, notified a condition variable the sender was
    // not waiting on, and then joined it. The socket was a local of the sender, so
    // nothing else could close it. `~RaftPeerTransport` blocked forever and the
    // node died to SIGKILL.
    //
    // On the reactor the write suspends, and closing the socket completes it --
    // which is a property the thread-per-peer design could not have had.
    Harness harness;
    harness.Start();
    harness.record->parkWrites = true;

    harness.transport->Send("n2", Vote(1));
    harness.reactor.Drain();

    // The sender is inside the write, and nothing the peer does will end it.
    REQUIRE(harness.transport->SendersRunning() == 1);
    REQUIRE(harness.Writes() == 0);

    harness.RequestStopAndDrain();

    CHECK(harness.transport->SendersRunning() == 0);
    CHECK(harness.reactor.PendingSubmissions() == 0);
    CHECK(harness.reactor.PendingTimers() == 0);
}

TEST_CASE("A dropped connection is redialled", "[consensus][raft][transport]")
{
    constexpr auto Backoff = 10ms;
    Harness harness;
    harness.Start(PeerTransportOptions { .dialTimeout = 50ms, .reconnectBackoff = Backoff, .stopWakeBound = 5ms });

    harness.transport->Send("n2", Vote(1));
    harness.reactor.Drain();
    REQUIRE(harness.Writes() == 1);
    auto const firstAttempts = harness.connector.Attempts();

    // Fail one write, which ends the session; then let the backoff elapse.
    harness.record->failWrites = true;
    harness.transport->Send("n2", Vote(2));
    harness.reactor.Drain();
    harness.clock.Advance(Backoff);
    harness.reactor.Drain();

    CHECK(harness.connector.Attempts() > firstAttempts);

    // And the reconnected sender writes again. No race to engineer around: the
    // old version of this case had a comment describing one, and it no longer
    // exists because nothing here runs on a thread of its own.
    harness.record->failWrites = false;
    harness.transport->Send("n2", Vote(3));
    harness.reactor.Drain();
    CHECK(harness.Writes() == 2);

    harness.RequestStopAndDrain(5ms);
}

TEST_CASE("ConnectedPeers is exact across a reconnect, and zero after teardown", "[consensus][raft][transport]")
{
    // The RAII guard's property. A count kept by hand is decremented at each of
    // the session's exits, and the one that gets forgotten makes this report a
    // fleet talking to peers it is not talking to -- which a readiness probe
    // reads as healthy.
    constexpr auto Backoff = 10ms;
    Harness harness;
    harness.Start(PeerTransportOptions { .dialTimeout = 50ms, .reconnectBackoff = Backoff, .stopWakeBound = 5ms });

    CHECK(harness.transport->ConnectedPeers() == 1);

    harness.record->failWrites = true;
    harness.transport->Send("n2", Vote(1));
    harness.reactor.Drain();
    CHECK(harness.transport->ConnectedPeers() == 0);

    harness.record->failWrites = false;
    harness.clock.Advance(Backoff);
    harness.reactor.Drain();
    CHECK(harness.transport->ConnectedPeers() == 1);

    harness.RequestStopAndDrain(5ms);
    CHECK(harness.transport->ConnectedPeers() == 0);
}

TEST_CASE("Stop is idempotent and safe before Start", "[consensus][raft][transport]")
{
    auto record = std::make_shared<RecordingSocket::Record>();
    ManualClock clock;
    TestReactor reactor { clock };
    ScriptedConnector connector { record };
    NullLogger logger;

    RaftPeerTransport transport { "n1", OnePeer(), reactor, connector, logger };
    transport.Stop();
    transport.Stop();
    CHECK(transport.SendersRunning() == 0);
}

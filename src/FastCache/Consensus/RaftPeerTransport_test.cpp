// SPDX-License-Identifier: Apache-2.0
//
// The outbound transport. Every case here drives a scripted connector rather
// than a socket: the properties worth pinning are about queueing, dropping and
// shutdown, and a real network would make each of them a race.
#include <FastCache/Consensus/RaftPeerTransport.hpp>
#include <FastCache/Consensus/RaftWire.hpp>
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
    [[nodiscard]] std::expected<std::unique_ptr<ISocket>, NetError> Connect(std::string_view host,
                                                                            std::uint16_t port,
                                                                            std::chrono::milliseconds connectTimeout,
                                                                            std::chrono::milliseconds ioTimeout) override
    {
        std::ignore = host;
        std::ignore = port;
        std::ignore = connectTimeout;
        std::ignore = ioTimeout;
        _attempts.fetch_add(1, std::memory_order_relaxed);
        if (_refuse.load(std::memory_order_relaxed))
            return std::unexpected { NetError {
                .code = NetErrorCode::ConnRefused, .systemCode = 0, .context = "scripted refusal" } };
        return std::make_unique<RecordingSocket>(_record);
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

/// Wait until `record` holds at least `count` writes, or give up.
/// @param record The write log.
/// @param count How many writes to wait for.
/// @return Whether they arrived.
[[nodiscard]] bool WaitForWrites(RecordingSocket::Record& record, std::size_t count)
{
    std::unique_lock lock { record.mutex };
    // Bounded rather than an unconditional wait: a helper that spins on a
    // counter a regression never advances hangs instead of failing, which is the
    // least useful way a suite can report a defect.
    return record.wake.wait_for(lock, 5s, [&record, count] { return record.writes.size() >= count; });
}

/// Spin until `predicate` holds, or give up.
///
/// Bounded rather than unconditional: a helper that spins on a condition a
/// regression never reaches hangs instead of failing, and a suite timeout naming
/// nothing is the least useful way a defect can be reported.
/// @param predicate What to wait for.
/// @return Whether it became true in time.
template <typename Predicate>
[[nodiscard]] bool WaitFor(Predicate predicate)
{
    auto const deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (predicate())
            return true;
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

/// One peer pointing anywhere; the scripted connector ignores the address.
/// @return The endpoint table.
[[nodiscard]] std::vector<PeerEndpoint> OnePeer()
{
    return { PeerEndpoint { .id = "n2", .host = "unused", .port = 1 } };
}

} // namespace

TEST_CASE("A sent message reaches the peer as a decodable frame", "[consensus][raft][transport]")
{
    auto record = std::make_shared<RecordingSocket::Record>();
    ScriptedConnector connector { record };
    NullLogger logger;

    RaftPeerTransport transport { "n1", OnePeer(), connector, logger };
    transport.Start();
    transport.Send("n2", Vote(7));

    REQUIRE(WaitForWrites(*record, 1));

    std::scoped_lock const guard { record->mutex };
    REQUIRE(record->writes.size() == 1);

    // Asserted by decoding rather than by byte count: what matters is that the
    // peer can read what arrived, which a length check would not establish.
    auto const header = RaftWire::DecodeHeader(record->writes[0]);
    REQUIRE(header.has_value());
    auto const decoded =
        RaftWire::DecodeMessage(header.value_or(RaftWire::FrameHeader {}),
                                std::span<std::byte const> { record->writes[0] }.subspan(RaftWire::HeaderSize));
    REQUIRE(decoded.has_value());
    auto const message = decoded.value_or(RaftMessage {});
    REQUIRE(std::holds_alternative<RequestVoteResponse>(message));
    CHECK(std::get<RequestVoteResponse>(message).term == Term { .value = 7 });
}

TEST_CASE("A message for this node itself is not sent anywhere", "[consensus][raft][transport]")
{
    // A configuration listing every member uniformly is the natural thing to
    // write, so the transport tolerates its own id rather than making each
    // caller filter -- but it must not give itself a socket and a sender thread.
    auto record = std::make_shared<RecordingSocket::Record>();
    ScriptedConnector connector { record };
    NullLogger logger;

    std::vector<PeerEndpoint> peers { PeerEndpoint { .id = "n1", .host = "self", .port = 1 },
                                      PeerEndpoint { .id = "n2", .host = "unused", .port = 2 } };
    RaftPeerTransport transport { "n1", std::move(peers), connector, logger };
    transport.Start();

    transport.Send("n1", Vote(1));
    transport.Send("n2", Vote(2));
    REQUIRE(WaitForWrites(*record, 1));

    std::scoped_lock const guard { record->mutex };
    // Exactly one write: the peer's. The self-addressed one was dropped.
    CHECK(record->writes.size() == 1);
}

TEST_CASE("A message for an unknown peer is dropped and counted", "[consensus][raft][transport]")
{
    // Counted rather than thrown. This is reached from the driver's send loop,
    // and a throw there would take a node down over a configuration gap that
    // costs it one peer.
    auto record = std::make_shared<RecordingSocket::Record>();
    ScriptedConnector connector { record };
    NullLogger logger;

    RaftPeerTransport transport { "n1", OnePeer(), connector, logger };
    transport.Start();
    transport.Send("nobody", Vote(1));

    CHECK(transport.DroppedMessages() == 1);
}

TEST_CASE("A queue for an unreachable peer is bounded", "[consensus][raft][transport]")
{
    // The property that keeps a peer being down from becoming a memory leak: an
    // hour of heartbeats for an unreachable follower must not accumulate.
    auto record = std::make_shared<RecordingSocket::Record>();
    ScriptedConnector connector { record };
    connector.Refuse(true);
    NullLogger logger;

    constexpr std::size_t Bound = 4;
    RaftPeerTransport transport { "n1",
                                  OnePeer(),
                                  connector,
                                  logger,
                                  PeerTransportOptions {
                                      .dialTimeout = 50ms, .reconnectBackoff = 10s, .maxQueuedPerPeer = Bound } };
    transport.Start();

    constexpr std::size_t Sent = 40;
    for (auto index = std::size_t { 0 }; index < Sent; ++index)
        transport.Send("n2", Vote(index));

    // Everything past the bound was dropped, so the drop count is what the queue
    // refused to hold rather than zero.
    CHECK(transport.DroppedMessages() >= Sent - Bound);
    CHECK(transport.ConnectedPeers() == 0);
}

TEST_CASE("Send does not block on an unreachable peer", "[consensus][raft][transport]")
{
    // The contract IRaftTransport states in as many words: a driver that waited
    // for a send would let one unreachable follower stall a leader that has a
    // quorum without it, turning a fault Raft tolerates into one it does not.
    auto record = std::make_shared<RecordingSocket::Record>();
    ScriptedConnector connector { record };
    connector.Refuse(true);
    NullLogger logger;

    RaftPeerTransport transport {
        "n1", OnePeer(), connector, logger, PeerTransportOptions { .dialTimeout = 10s, .reconnectBackoff = 10s }
    };
    transport.Start();

    auto const started = std::chrono::steady_clock::now();
    for (auto index = 0; index < 100; ++index)
        transport.Send("n2", Vote(1));
    auto const elapsed = std::chrono::steady_clock::now() - started;

    // Generously bounded: the assertion is "it returned rather than waiting on a
    // ten-second dial", not a latency measurement.
    CHECK(elapsed < 2s);
}

TEST_CASE("Stopping is prompt even while a peer is unreachable", "[consensus][raft][transport]")
{
    // A backoff that cannot be interrupted makes shutdown take as long as the
    // interval, and an uninterruptible dial makes it take as long as the OS
    // allows. Both are how a service ends up killed by its supervisor instead of
    // stopping -- the lesson `WorkerServer::Run` records for the accept side.
    auto record = std::make_shared<RecordingSocket::Record>();
    ScriptedConnector connector { record };
    connector.Refuse(true);
    NullLogger logger;

    auto const started = std::chrono::steady_clock::now();
    {
        RaftPeerTransport transport {
            "n1", OnePeer(), connector, logger, PeerTransportOptions { .dialTimeout = 50ms, .reconnectBackoff = 30s }
        };
        transport.Start();

        // Let the sender reach its backoff wait rather than racing it to the
        // first dial, so the case actually exercises the interruption.
        std::this_thread::sleep_for(100ms);
    }
    auto const elapsed = std::chrono::steady_clock::now() - started;

    // Far below the 30s backoff: the wait was interrupted, not waited out.
    CHECK(elapsed < 5s);
}

TEST_CASE("A dropped connection is redialled", "[consensus][raft][transport]")
{
    // Recovery is the whole point of a peer transport: a partition heals and
    // replication must resume without anybody restarting a node.
    auto record = std::make_shared<RecordingSocket::Record>();
    ScriptedConnector connector { record };
    NullLogger logger;

    RaftPeerTransport transport {
        "n1", OnePeer(), connector, logger, PeerTransportOptions { .dialTimeout = 50ms, .reconnectBackoff = 10ms }
    };
    transport.Start();

    transport.Send("n2", Vote(1));
    REQUIRE(WaitForWrites(*record, 1));
    auto const firstAttempts = connector.Attempts();

    // Break the connection under the sender.
    {
        std::scoped_lock const guard { record->mutex };
        record->failWrites = true;
    }
    transport.Send("n2", Vote(2));

    // Wait for the REDIAL rather than for a duration. Clearing the failure after
    // a fixed sleep is what the first version of this case did, and it raced:
    // the sender had not yet consumed the message, so the write succeeded and
    // the reconnect the case exists to observe never happened -- a test that
    // passed while proving nothing.
    REQUIRE(WaitFor([&] { return connector.Attempts() > firstAttempts; }));

    {
        std::scoped_lock const guard { record->mutex };
        record->failWrites = false;
    }

    // The failed write cost that message and the connection; the next send must
    // still get through, on the fresh one.
    transport.Send("n2", Vote(3));
    REQUIRE(WaitForWrites(*record, 2));
}

TEST_CASE("Stop is idempotent and safe before Start", "[consensus][raft][transport]")
{
    auto record = std::make_shared<RecordingSocket::Record>();
    ScriptedConnector connector { record };
    NullLogger logger;

    RaftPeerTransport transport { "n1", OnePeer(), connector, logger };
    transport.Stop();
    transport.Stop();

    // And a send after stopping is a no-op rather than a queue that nobody will
    // ever drain.
    transport.Send("n2", Vote(1));
    CHECK(transport.ConnectedPeers() == 0);
}

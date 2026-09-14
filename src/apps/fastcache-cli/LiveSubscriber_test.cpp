// SPDX-License-Identifier: Apache-2.0
#include "LiveSubscriber.hpp"
#include "NodeClient.hpp"
#include "SocketExchange.hpp"

#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/WireFields.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Metrics/StatsReadingCodec.hpp>
#include <FastCache/Net/BlockingSocket.hpp>
#include <FastCache/Net/TcpClient.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Cli;
using namespace std::chrono_literals;
using FastCache::Testing::Unwrap;

namespace Wire = FastCache::CompileCacheWire;

namespace
{

/// A reading with one counter moved, so a round trip that lost the counters cannot compare equal.
/// @return The reading.
[[nodiscard]] StatsReading SomeReading()
{
    AtomicMetricsSink sink;
    sink.Increment(IMetricsSink::Counter::LiveSubscriptionsOpened, 7);
    return CaptureStatsReading(sink, MetricsSnapshot {});
}

/// @param payload A push's payload.
/// @return The reply a stream carries it in.
[[nodiscard]] NodeReply Push(std::vector<std::byte> payload)
{
    return NodeReply { .status = Wire::Status::Push, .payload = std::move(payload) };
}

/// @param subject What was granted.
/// @param layout The layout the grant states.
/// @return The grant, as a reply.
[[nodiscard]] NodeReply Grant(Wire::LiveSubject subject, std::uint64_t layout)
{
    return Push(Wire::EncodeLiveSubscribed(Wire::LiveSubscribedFields {
        .subject = subject, .grantedCadenceMillis = 500, .statsLayout = layout, .endpoint = "build-07:7070" }));
}

/// @param body A snapshot's body.
/// @return The snapshot at tick 42, as a reply.
[[nodiscard]] NodeReply Snapshot(std::span<std::byte const> body)
{
    return Push(Wire::EncodeLiveSnapshot(42, body));
}

/// A node's status naming @p version.
/// @param version What it names.
/// @return The status.
[[nodiscard]] Wire::NodeStatusFields StatusNaming(std::string version)
{
    auto status = Wire::NodeStatusFields {};
    status.version = std::move(version);
    return status;
}

} // namespace

TEST_CASE("A grant is read, and one this client cannot read the stream of is refused by name",
          "[cli][livestats][subscription]")
{
    SECTION("the subject asked for, in this build's layout")
    {
        auto const frame = ReadLiveFrame(Wire::LiveSubject::Cache, Grant(Wire::LiveSubject::Cache, StatsReadingLayout));
        REQUIRE(frame.kind == LiveFrameKind::Granted);
        CHECK(frame.granted.grantedCadenceMillis == 500);
        CHECK(frame.granted.endpoint == "build-07:7070");
    }

    SECTION("another build's layout, named with both digests")
    {
        auto const frame = ReadLiveFrame(Wire::LiveSubject::Node, Grant(Wire::LiveSubject::Node, StatsReadingLayout ^ 0x1U));
        REQUIRE(frame.kind == LiveFrameKind::Unreadable);
        CHECK(frame.note.contains("layout"));
        CHECK(frame.note.contains(std::format("{:#018x}", StatsReadingLayout)));
        CHECK(frame.note.contains("build-07:7070"));
    }

    SECTION("a fleet stream states no stats layout, and one that does is not a fleet stream")
    {
        CHECK(ReadLiveFrame(Wire::LiveSubject::Fleet, Grant(Wire::LiveSubject::Fleet, 0)).kind == LiveFrameKind::Granted);
        CHECK(ReadLiveFrame(Wire::LiveSubject::Fleet, Grant(Wire::LiveSubject::Fleet, StatsReadingLayout)).kind
              == LiveFrameKind::Unreadable);
    }

    SECTION("a subject other than the one asked for")
    {
        auto const frame = ReadLiveFrame(Wire::LiveSubject::Node, Grant(Wire::LiveSubject::Cache, StatsReadingLayout));
        REQUIRE(frame.kind == LiveFrameKind::Unreadable);
        CHECK(frame.note.contains("a node subscription was granted a cache stream"));
    }
}

TEST_CASE("A reading is decoded in its subject's grammar into the model /metrics renders from",
          "[cli][livestats][subscription]")
{
    auto const reading = SomeReading();
    auto const encoded = EncodeStatsReading(reading);

    SECTION("cache: the reading alone")
    {
        auto const frame = ReadLiveFrame(Wire::LiveSubject::Cache, Snapshot(encoded));
        REQUIRE(frame.kind == LiveFrameKind::Reading);
        CHECK(frame.tick == 42);
        REQUIRE(frame.reading.has_value());
        CHECK(Unwrap(frame.reading) == reading);
        CHECK_FALSE(frame.nodeStatus.has_value());
    }

    SECTION("node: the reading, then the node's status on the same tick")
    {
        auto const status = Wire::EncodeNodeStatus(StatusNaming("0.4.1"));
        auto const body =
            WireFields::Encode({ std::span<std::byte const> { encoded }, std::span<std::byte const> { status } });
        auto const frame = ReadLiveFrame(Wire::LiveSubject::Node, Snapshot(body));
        REQUIRE(frame.kind == LiveFrameKind::Reading);
        REQUIRE(frame.reading.has_value());
        CHECK(Unwrap(frame.reading) == reading);
        REQUIRE(frame.nodeStatus.has_value());
        CHECK(Unwrap(frame.nodeStatus).version == "0.4.1");
    }

    SECTION("node: a body that is one field, not two, is not read as a cache reading")
    {
        CHECK(ReadLiveFrame(Wire::LiveSubject::Node, Snapshot(encoded)).kind == LiveFrameKind::Unreadable);
    }

    SECTION("fleet: the leader's document, whole")
    {
        auto const text = std::string { "## fleet\nmachines 12\n" };
        auto const frame = ReadLiveFrame(Wire::LiveSubject::Fleet, Snapshot(Wire::AsBytes(text)));
        REQUIRE(frame.kind == LiveFrameKind::Reading);
        CHECK(frame.document == text);
        CHECK_FALSE(frame.reading.has_value());
    }

    SECTION("a reading whose own digest is another build's is refused, whatever the grant said")
    {
        auto foreign = encoded;
        foreign.front() ^= std::byte { 0x01 };
        auto const frame = ReadLiveFrame(Wire::LiveSubject::Cache, Snapshot(foreign));
        REQUIRE(frame.kind == LiveFrameKind::Unreadable);
        CHECK(frame.note.contains("a cache reading is laid out by a build other than this client's"));
    }
}

TEST_CASE("A stream's other frames say what they are, and an answer no stream sends is unreadable",
          "[cli][livestats][subscription]")
{
    SECTION("a gap counts what was dropped")
    {
        auto const frame =
            ReadLiveFrame(Wire::LiveSubject::Cache,
                          Push(Wire::EncodeLiveGap(Wire::LiveGapFields { .dropped = 3, .firstTick = 10, .lastTick = 12 })));
        REQUIRE(frame.kind == LiveFrameKind::Gap);
        CHECK(frame.dropped == 3);
    }

    SECTION("an event names what changed")
    {
        auto const frame = ReadLiveFrame(Wire::LiveSubject::Fleet,
                                         Push(Wire::EncodeLiveEvent(Wire::LiveEventFields {
                                             .kind = Wire::LiveEventKind::MemberJoined, .detail = "build-09" })));
        REQUIRE(frame.kind == LiveFrameKind::Event);
        CHECK(frame.event.kind == Wire::LiveEventKind::MemberJoined);
        CHECK(frame.event.detail == "build-09");
    }

    SECTION("Ok is the server ending the stream in order")
    {
        CHECK(ReadLiveFrame(Wire::LiveSubject::Cache, NodeReply { .status = Wire::Status::Ok }).kind
              == LiveFrameKind::Ended);
    }

    SECTION("a refusal keeps its code and the server's words, which for NotLeader are an address")
    {
        auto const frame = ReadLiveFrame(Wire::LiveSubject::Fleet,
                                         NodeReply { .status = Wire::Status::Error,
                                                     .payload = {},
                                                     .code = Wire::ErrorCode::NotLeader,
                                                     .detail = "build-01:7071" });
        REQUIRE(frame.kind == LiveFrameKind::Refused);
        CHECK(frame.code == Wire::ErrorCode::NotLeader);
        CHECK(frame.note == "build-01:7071");
    }

    SECTION("a miss")
    {
        CHECK(ReadLiveFrame(Wire::LiveSubject::Cache, NodeReply { .status = Wire::Status::Miss }).kind
              == LiveFrameKind::Unreadable);
    }
}

namespace
{

/// Await one accept; a blocking listener resolves it synchronously.
/// @param listener The bound listener.
/// @return The accepted socket, or the accept error.
[[nodiscard]] Task<AcceptResult> AcceptOne(BlockingListener* listener)
{
    co_return co_await listener->Accept();
}

/// One read, as a task `SyncRun` can drive.
/// @param socket The socket.
/// @param buffer Where the bytes go; not empty.
/// @return Bytes read, `0` for EOF, or the failure.
[[nodiscard]] Task<IoResult> ReadSome(ISocket* socket, std::span<std::byte> buffer)
{
    co_return co_await socket->Read(buffer);
}

/// What the stand-in node observed.
struct ServerRecord
{
    std::optional<Wire::LiveSubject> subject {}; ///< What the SUBSCRIBE asked for.
    bool wrote { false };                        ///< Whether both frames went out.
    bool sawEof { false };                       ///< Whether the client's leaving arrived as EOF.
    bool sawError { false };                     ///< Whether it arrived as an error instead: a reset.
};

/// Serve one subscription the way a node does up to its first snapshot, then wait for the watcher to leave.
///
/// Every wait is bounded by the socket's own receive deadline, so a client that never leaves ends
/// this with neither flag set rather than hanging the case.
/// @param listener Where the client dials.
/// @param record Where what happened is written.
void ServeOneStream(BlockingListener* listener, ServerRecord* record)
{
    auto accepted = SyncRun(AcceptOne(listener));
    if (!accepted.has_value())
        return;
    auto const socket = *std::move(accepted);
    socket->SetReceiveDeadline(5s);

    auto const header = SyncRun(RecvExactly(socket.get(), Wire::RequestHeaderSize));
    auto const decoded = header.has_value() ? Wire::DecodeRequestHeader(*header) : std::nullopt;
    if (!decoded.has_value())
        return;
    auto const payload = SyncRun(RecvExactly(socket.get(), decoded->payloadLength));
    auto const request = payload.has_value() ? Wire::DecodeSubscribeRequest(*payload) : std::nullopt;
    if (!request.has_value())
        return;
    record->subject = request->subject;

    auto const reading = EncodeStatsReading(SomeReading());
    auto const granted =
        Wire::EncodeReply(Wire::Status::Push,
                          Wire::EncodeLiveSubscribed(Wire::LiveSubscribedFields { .subject = request->subject,
                                                                                  .grantedCadenceMillis = 500,
                                                                                  .statsLayout = StatsReadingLayout,
                                                                                  .endpoint = "127.0.0.1" }));
    auto const snapshot = Wire::EncodeReply(Wire::Status::Push, Wire::EncodeLiveSnapshot(1, reading));
    record->wrote = SyncRun(SendAll(socket.get(), granted)) && SyncRun(SendAll(socket.get(), snapshot));

    auto buffer = std::array<std::byte, 64> {};
    auto const got = SyncRun(ReadSome(socket.get(), buffer));
    record->sawEof = got.has_value() && *got == 0;
    record->sawError = !got.has_value();
    socket->Close();
}

} // namespace

TEST_CASE("A subscription reads its stream, and leaving it while a read blocks is the node's goodbye",
          "[cli][livestats][subscription]")
{
    auto listener = BlockingListener::Bind("127.0.0.1", 0);
    if (listener == nullptr || !listener->IsBound())
        SKIP("this host would not bind a loopback listener");
    listener->SetTimeouts(5s, 5s);

    auto record = ServerRecord {};
    auto subscription = NodeSubscription { DialTimeouts { .connect = 5s, .io = 5s }, Credential {} };
    {
        // Declared after what it writes to, so an unwinding case joins it before those go.
        auto const server = std::jthread { [&listener, &record] { ServeOneStream(listener.get(), &record); } };

        auto const opened = subscription.Open(
            Endpoint { .host = "127.0.0.1", .port = listener->BoundPort() },
            Wire::SubscribeRequest { .subject = Wire::LiveSubject::Node, .cadenceMillis = 500, .dashboardToken = {} });
        REQUIRE(opened.has_value());

        auto const grant = subscription.Read();
        REQUIRE(grant.has_value());
        auto const granted = ReadLiveFrame(Wire::LiveSubject::Node, *grant);
        REQUIRE(granted.kind == LiveFrameKind::Granted);
        subscription.ExpectEvery(std::chrono::milliseconds { granted.granted.grantedCadenceMillis });

        auto const first = subscription.Read();
        REQUIRE(first.has_value());
        CHECK(first->status == Wire::Status::Push);

        // From another thread, as a session's close is: in either order against the read below, the
        // half-close makes the node close, and that close is what ends the read.
        auto const leaver = std::jthread { [&subscription] { subscription.Leave(); } };
        auto const after = subscription.Read();
        REQUIRE_FALSE(after.has_value());
        // In a stream's words: `closed the connection without answering` after a grant and a snapshot names a
        // reply the subscription never asked for.
        CHECK(after.error().detail == "the server closed the stream");
    }

    CHECK(record.subject == Wire::LiveSubject::Node);
    CHECK(record.wrote);
    CHECK(record.sawEof);
    CHECK_FALSE(record.sawError);

    // A session that left does not dial again.
    auto const again = subscription.Open(
        Endpoint { .host = "127.0.0.1", .port = listener->BoundPort() },
        Wire::SubscribeRequest { .subject = Wire::LiveSubject::Node, .cadenceMillis = 500, .dashboardToken = {} });
    CHECK_FALSE(again.has_value());
}

TEST_CASE("A leave that arrives while the subscription dials is still the node's goodbye once the dial connects",
          "[cli][livestats][subscription]")
{
    // WHAT DISTINGUISHES: the session closes while the dial is out, so `Leave` finds no exchange to half-close.
    // The half-close is then owed by `Open` once the dial connects. Without it the node streams into a socket
    // nobody reads until the subscription is destroyed, its read waits out its deadline, and it counts a reset
    // rather than the goodbye: `sawEof` false and `sawError` true.
    auto listener = BlockingListener::Bind("127.0.0.1", 0);
    if (listener == nullptr || !listener->IsBound())
        SKIP("this host would not bind a loopback listener");
    listener->SetTimeouts(5s, 5s);

    auto record = ServerRecord {};
    NodeSubscription* self = nullptr;
    auto leavesWhileDialling = [&self](Endpoint const& endpoint, DialTimeouts timeouts, Credential const& credential) {
        self->Leave();
        return NodeExchange::Open(endpoint, timeouts, credential);
    };
    auto subscription =
        NodeSubscription { DialTimeouts { .connect = 5s, .io = 5s }, Credential {}, std::move(leavesWhileDialling) };
    self = &subscription;
    {
        auto const server = std::jthread { [&listener, &record] { ServeOneStream(listener.get(), &record); } };
        auto const opened = subscription.Open(
            Endpoint { .host = "127.0.0.1", .port = listener->BoundPort() },
            Wire::SubscribeRequest { .subject = Wire::LiveSubject::Cache, .cadenceMillis = 500, .dashboardToken = {} });
        REQUIRE(opened.has_value());
    }

    CHECK(record.subject == Wire::LiveSubject::Cache);
    CHECK(record.sawEof);
    CHECK_FALSE(record.sawError);
}

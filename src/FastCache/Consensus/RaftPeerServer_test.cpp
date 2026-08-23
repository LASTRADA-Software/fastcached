// SPDX-License-Identifier: Apache-2.0
//
// The inbound peer listener. The case that matters most is the one about a
// frame this build cannot interpret: it must be stepped over rather than end
// the connection, because a node running a newer build would otherwise
// partition itself from every older peer in a fleet nobody upgrades atomically.
#include <FastCache/Consensus/RaftPeerServer.hpp>
#include <FastCache/Consensus/RaftWire.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Net/InMemoryTransport.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace FastCache;
using namespace FastCache::Consensus;

namespace
{

/// Records what the server decoded.
class RecordingSink final: public IRaftMessageSink
{
  public:
    /// @copydoc IRaftMessageSink::Deliver
    void Deliver(RaftMessage message) override
    {
        received.push_back(std::move(message));
    }

    std::vector<RaftMessage> received; ///< In arrival order.
};

/// A vote response carrying `term`, the smallest message that round-trips.
/// @param term Term to carry.
/// @return The framed message.
[[nodiscard]] std::vector<std::byte> VoteFrame(std::uint64_t term)
{
    return RaftWire::Encode(RaftMessage {
        RequestVoteResponse { .term = Term { .value = term }, .decision = VoteDecision::Granted, .voterId = "n2" } });
}

/// Write every byte of `bytes` to `socket`.
///
/// By pointer, not reference: a coroutine's reference parameter is bound before
/// the first suspension and outlives the frame that kept it alive.
/// @param socket Destination; never null.
/// @param bytes What to send.
/// @return Task resolving when the bytes are queued for the peer.
[[nodiscard]] Task<void> WriteAll(ISocket* socket, std::span<std::byte const> bytes)
{
    std::size_t sent = 0;
    while (sent < bytes.size())
    {
        auto const written = co_await socket->Write(bytes.subspan(sent));
        if (!written.has_value() || *written == 0)
            co_return;
        sent += *written;
    }
}

/// Append `bytes` to `out`.
/// @param out Destination.
/// @param bytes What to append.
void Append(std::vector<std::byte>& out, std::span<std::byte const> bytes)
{
    out.insert(out.end(), bytes.begin(), bytes.end());
}

/// Drive a server over one in-memory connection carrying `wire`, to completion.
///
/// The client half writes everything and then closes, so the server's reader
/// sees a clean EOF and its connection coroutine finishes — which is what lets
/// each case assert on a settled result rather than racing one.
/// @param wire The bytes a peer sends.
/// @param sink Where decoded messages land.
/// @param options Limits to run the server under.
/// @return The server, after its accept loop has ended.
[[nodiscard]] std::unique_ptr<RaftPeerServer> RunOnce(std::vector<std::byte> const& wire,
                                                      RecordingSink& sink,
                                                      PeerServerOptions options = {})
{
    InMemoryListener listener;
    NullLogger logger;
    auto server = std::make_unique<RaftPeerServer>(listener, sink, logger, options);

    auto client = listener.ConnectClient();
    SyncRun(WriteAll(client.get(), wire));

    // Closed before the server runs, so its reader sees a clean EOF and its
    // connection coroutine finishes rather than parking -- which is what lets
    // each case assert on a settled result instead of racing one.
    client->Close();

    // The listener drains queued connections before reporting itself closed, so
    // closing here still delivers the one connection and then ends the loop.
    listener.Close();
    SyncRun(server->Run());
    return server;
}

} // namespace

TEST_CASE("A framed message is decoded and delivered", "[consensus][raft][peerserver]")
{
    RecordingSink sink;
    auto const server = RunOnce(VoteFrame(7), sink);

    REQUIRE(sink.received.size() == 1);
    REQUIRE(std::holds_alternative<RequestVoteResponse>(sink.received[0]));
    CHECK(std::get<RequestVoteResponse>(sink.received[0]).term == Term { .value = 7 });
    CHECK(server->DeliveredMessages() == 1);
}

TEST_CASE("Several messages on one connection all arrive", "[consensus][raft][peerserver]")
{
    // A peer connection is long-lived and carries a stream, so reading exactly
    // one frame per connection would work in every single-message test and
    // deliver one heartbeat per reconnect in a real cluster.
    std::vector<std::byte> wire;
    for (auto term = std::uint64_t { 1 }; term <= 5; ++term)
        Append(wire, VoteFrame(term));

    RecordingSink sink;
    auto const server = RunOnce(wire, sink);

    REQUIRE(sink.received.size() == 5);
    for (auto index = std::size_t { 0 }; index < 5; ++index)
        CHECK(std::get<RequestVoteResponse>(sink.received[index]).term == Term { .value = index + 1 });
    CHECK(server->DeliveredMessages() == 5);
}

TEST_CASE("An unknown message type is stepped over, not fatal", "[consensus][raft][peerserver]")
{
    // The mixed-fleet property. The frame between the two known ones carries a
    // type this build has never heard of; the one after it must still arrive.
    auto unknown = VoteFrame(2);
    unknown[2] = std::byte { 0x7F };

    std::vector<std::byte> wire;
    Append(wire, VoteFrame(1));
    Append(wire, unknown);
    Append(wire, VoteFrame(3));

    RecordingSink sink;
    auto const server = RunOnce(wire, sink);

    REQUIRE(sink.received.size() == 2);
    CHECK(std::get<RequestVoteResponse>(sink.received[0]).term == Term { .value = 1 });
    CHECK(std::get<RequestVoteResponse>(sink.received[1]).term == Term { .value = 3 });
    CHECK(server->SkippedFrames() == 1);
}

TEST_CASE("A frame from a newer protocol version is stepped over too", "[consensus][raft][peerserver]")
{
    // Same property, reached the other way: the type is known and the version is
    // not. Both are "a peer running another build", and both must leave the
    // connection usable.
    auto const newer =
        RaftWire::Encode(RaftMessage { RequestVoteResponse {
                             .term = Term { .value = 9 }, .decision = VoteDecision::Denied, .voterId = "n2" } },
                         RaftWire::CurrentVersion + 1);

    std::vector<std::byte> wire;
    Append(wire, newer);
    Append(wire, VoteFrame(4));

    RecordingSink sink;
    auto const server = RunOnce(wire, sink);

    REQUIRE(sink.received.size() == 1);
    CHECK(std::get<RequestVoteResponse>(sink.received[0]).term == Term { .value = 4 });
    CHECK(server->SkippedFrames() == 1);
}

TEST_CASE("A wrong magic ends the connection", "[consensus][raft][peerserver]")
{
    // The one condition under which the reader cannot find where the frame ends,
    // so there is nothing to resynchronize to. The frame after it must NOT be
    // delivered -- reading on would be guessing at where it starts.
    auto bad = VoteFrame(1);
    bad[0] = std::byte { 0xFC };

    std::vector<std::byte> wire;
    Append(wire, bad);
    Append(wire, VoteFrame(2));

    RecordingSink sink;
    auto const server = RunOnce(wire, sink);

    CHECK(sink.received.empty());
    CHECK(server->SkippedFrames() == 0);
}

TEST_CASE("A malformed payload ends the connection", "[consensus][raft][peerserver]")
{
    // Distinct from an unknown type on purpose: the header decoded and the
    // payload was consumed, so the reader is still in sync -- but the sender and
    // this reader disagree about what the bytes mean, which no later frame
    // repairs.
    auto bad = VoteFrame(1);
    // Corrupt the vote decision to a value naming no enumerator. The payload's
    // first field is the term (4-byte prefix + 8 bytes), so the decision's own
    // prefix follows it and its single byte comes after that.
    bad[RaftWire::HeaderSize + 4 + 8 + 4] = std::byte { 0x7F };

    std::vector<std::byte> wire;
    Append(wire, bad);
    Append(wire, VoteFrame(2));

    RecordingSink sink;
    auto const server = RunOnce(wire, sink);

    CHECK(sink.received.empty());
    CHECK(server->SkippedFrames() == 0);
}

TEST_CASE("An over-large declared frame is refused before it is buffered", "[consensus][raft][peerserver]")
{
    // The declared length is a u32, so a peer can claim four gigabytes. Without
    // a cap the declaration IS the allocation, which makes one frame a
    // memory-exhaustion vector.
    auto oversized = VoteFrame(1);
    // Rewrite the declared payload length to something past the cap, leaving the
    // rest of the frame alone.
    oversized[3] = std::byte { 0x00 };
    oversized[4] = std::byte { 0x40 };
    oversized[5] = std::byte { 0x00 };
    oversized[6] = std::byte { 0x00 };

    RecordingSink sink;
    auto const server = RunOnce(oversized, sink, PeerServerOptions { .maxFrameBytes = 1024 });

    CHECK(sink.received.empty());
    CHECK(server->DeliveredMessages() == 0);
}

TEST_CASE("A truncated frame ends the connection without delivering", "[consensus][raft][peerserver]")
{
    // What a peer that died mid-write produces. Ordinary, and it must not be
    // half-decoded into a message.
    auto whole = VoteFrame(1);
    whole.resize(whole.size() - 3);

    RecordingSink sink;
    auto const server = RunOnce(whole, sink);

    CHECK(sink.received.empty());
    CHECK(server->DeliveredMessages() == 0);
}

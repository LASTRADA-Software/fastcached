// SPDX-License-Identifier: Apache-2.0
//
// The layer a proven connection is sealed under (#178), driven over an in-memory pair: which
// bytes reach the wire, which a reader is handed, and what ends a sealed connection. Where the
// keys come from is `Distributed::NodeProof`'s; the endpoint's use of this layer, a relayed
// handshake included, is `FrameEndpoint_test`'s.
#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/SessionSeal.hpp>
#include <FastCache/Core/WireFields.hpp>
#include <FastCache/Net/ISocket.hpp>
#include <FastCache/Net/InMemoryTransport.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>
#include <FastCache/Protocol/SealedFrameSocket.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace FastCache;

namespace
{

namespace Wire = CompileCacheWire;

/// The most payload the cases' sealed ends hold before they can check a tag.
constexpr std::size_t MaxPayload = 4096;

/// A session key derived from @p secret: two cases' keys differ exactly when their secrets do.
/// @param secret The shared secret.
/// @return The key.
[[nodiscard]] SessionKey Key(std::string_view secret)
{
    auto key =
        DeriveSessionKey(WireFields::AsBytes(secret), WireFields::AsBytes("both nonces"), WireFields::AsBytes("a wire"));
    REQUIRE(key.has_value());
    return *std::move(key);
}

/// One read from @p socket.
/// @param socket The socket.
/// @param buffer Where the bytes go.
/// @return What the read answered.
Task<IoResult> ReadOnce(ISocket* socket, std::span<std::byte> buffer)
{
    co_return co_await socket->read(buffer);
}

/// One write to @p socket.
/// @param socket The socket.
/// @param bytes What to write; owned, so the coroutine holds nothing it does not own.
/// @return What the write answered.
Task<IoResult> WriteOnce(ISocket* socket, std::vector<std::byte> bytes)
{
    co_return co_await socket->write(std::span<std::byte const> { bytes });
}

/// Everything @p socket holds now, in one read.
///
/// Every case writes before it reads, so the read completes at once: a read that would park is
/// a case waiting for bytes nobody sends, and `SyncRun` says so rather than hanging. It retrieves
/// that park through `CancelRead` BEFORE it says so (#178): the read parks inside the sealing
/// layer, which holds the awaitable of the frame `SyncRun` is about to free, so without the
/// retrieval a seal that stopped refusing ended the case in a SIGSEGV instead of a failure.
/// @param socket The socket.
/// @return The bytes, or what refused them.
[[nodiscard]] std::expected<std::vector<std::byte>, NetError> ReadAvailable(ISocket& socket)
{
    auto buffer = std::vector<std::byte>(4 * MaxPayload);
    auto const got = SyncRun(ReadOnce(&socket, buffer), [&socket] { socket.cancelRead(); });
    if (!got.has_value())
        return std::unexpected(got.error());
    buffer.resize(*got);
    return buffer;
}

/// Write @p bytes to @p socket in one call.
/// @param socket The socket.
/// @param bytes What to write.
/// @return How many bytes the write reported.
[[nodiscard]] std::size_t Written(ISocket& socket, std::span<std::byte const> bytes)
{
    auto const wrote = SyncRun(WriteOnce(&socket, std::vector<std::byte> { bytes.begin(), bytes.end() }));
    REQUIRE(wrote.has_value());
    return *wrote;
}

/// A request frame, as a sealed connection carries them.
/// @param key What it asks for; its length decides the payload's.
/// @return The frame.
[[nodiscard]] std::vector<std::byte> RequestFrame(std::string_view key = "ab")
{
    return Wire::EncodeFetch(key);
}

/// @p frame followed by the tag @p sealer gives it: what a sealed end puts on the wire.
/// @param sealer The sealer, at the frame's position.
/// @param frame A whole request frame.
/// @return The bytes.
[[nodiscard]] std::vector<std::byte> SealedRequest(FrameSealer& sealer, std::span<std::byte const> frame)
{
    auto const tag = sealer.Seal(frame.first(Wire::RequestHeaderSize), frame.subspan(Wire::RequestHeaderSize));
    auto bytes = std::vector<std::byte> { frame.begin(), frame.end() };
    bytes.insert(bytes.end(), tag.begin(), tag.end());
    return bytes;
}

/// @p frame followed by a tag nobody computed: what a relay without the session key can send.
/// @param frame A whole frame.
/// @return The bytes.
[[nodiscard]] std::vector<std::byte> ForgedRequest(std::span<std::byte const> frame)
{
    auto bytes = std::vector<std::byte> { frame.begin(), frame.end() };
    bytes.insert(bytes.end(), SessionTagBytes, std::byte { 0 });
    return bytes;
}

/// A server end over @p raw, reading requests sealed under @p secret's key.
/// @param raw The accepted socket.
/// @param secret Whose key.
/// @return The end.
[[nodiscard]] std::unique_ptr<SealedFrameSocket> SealedServer(std::unique_ptr<ISocket> raw, std::string_view secret)
{
    auto server = std::make_unique<SealedFrameSocket>(std::move(raw), SealedFrameEnd::Server, MaxPayload);
    server->SealReceiving(Key(secret));
    return server;
}

} // namespace

TEST_CASE("Before a key is agreed a sealing layer passes every byte through untouched", "[protocol][seal]")
{
    // Every connection on a surface that offers a proof is wrapped from accept, and a launcher on
    // that port never proves anything: it must see exactly the bytes it would have seen unwrapped.
    auto pair = InMemorySocketPair::Create();
    SealedFrameSocket caller { std::move(pair.client), SealedFrameEnd::Caller, MaxPayload };
    SealedFrameSocket server { std::move(pair.server), SealedFrameEnd::Server, MaxPayload };

    auto const frame = RequestFrame();
    REQUIRE(Written(caller, frame) == frame.size());
    CHECK(ReadAvailable(server) == frame);

    auto const reply = Wire::EncodeReply(Wire::Status::Ok, {});
    REQUIRE(Written(server, reply) == reply.size());
    CHECK(ReadAvailable(caller) == reply);

    CHECK_FALSE(caller.Sealed());
    CHECK_FALSE(server.Sealed());
}

TEST_CASE("A sealed frame reaches the wire with its tag and the reader without it", "[protocol][seal]")
{
    auto const frame = RequestFrame();

    SECTION("on the wire, the frame and then the tag for its position")
    {
        auto pair = InMemorySocketPair::Create();
        SealedFrameSocket caller { std::move(pair.client), SealedFrameEnd::Caller, MaxPayload };
        caller.SealSending(Key("caller-to-server"));

        REQUIRE(Written(caller, frame) == frame.size());
        REQUIRE(Written(caller, frame) == frame.size());

        // Two identical frames carry two DIFFERENT tags, because the position is in the MAC:
        // which is what makes a frame replayed inside the session fail.
        FrameSealer expected { Key("caller-to-server") };
        auto wire = SealedRequest(expected, frame);
        auto const second = SealedRequest(expected, frame);
        CHECK_FALSE(
            std::ranges::equal(std::span { wire }.last(SessionTagBytes), std::span { second }.last(SessionTagBytes)));
        wire.insert(wire.end(), second.begin(), second.end());
        CHECK(ReadAvailable(*pair.server) == wire);
    }

    SECTION("at the reader, the frame alone")
    {
        auto pair = InMemorySocketPair::Create();
        SealedFrameSocket caller { std::move(pair.client), SealedFrameEnd::Caller, MaxPayload };
        caller.SealSending(Key("caller-to-server"));
        auto const server = SealedServer(std::move(pair.server), "caller-to-server");

        REQUIRE(Written(caller, frame) == frame.size());
        CHECK(ReadAvailable(*server) == frame);
        CHECK_FALSE(server->Fault().has_value());
    }
}

TEST_CASE("A frame written in pieces is sealed once, when it is whole", "[protocol][seal]")
{
    // The endpoint writes a header and a payload as it has them; the tag is over both, so the
    // layer holds the first piece until the second arrives and reports every byte as taken.
    auto pair = InMemorySocketPair::Create();
    SealedFrameSocket caller { std::move(pair.client), SealedFrameEnd::Caller, MaxPayload };
    caller.SealSending(Key("caller-to-server"));
    auto const server = SealedServer(std::move(pair.server), "caller-to-server");

    auto const frame = RequestFrame("a-longer-key");
    auto const bytes = std::span<std::byte const> { frame };
    CHECK(Written(caller, bytes.first(3)) == 3);
    CHECK(Written(caller, bytes.subspan(3, Wire::RequestHeaderSize)) == Wire::RequestHeaderSize);
    CHECK(Written(caller, bytes.subspan(3 + Wire::RequestHeaderSize)) == frame.size() - 3 - Wire::RequestHeaderSize);

    CHECK(ReadAvailable(*server) == frame);
}

TEST_CASE("A reply is sealed by the server's end and opened by the caller's", "[protocol][seal]")
{
    // The two ends read DIFFERENT grammars -- a request header is seven bytes, a reply's five --
    // so a layer that framed replies as requests would locate the tag in the wrong place.
    auto pair = InMemorySocketPair::Create();
    SealedFrameSocket caller { std::move(pair.client), SealedFrameEnd::Caller, MaxPayload };
    SealedFrameSocket server { std::move(pair.server), SealedFrameEnd::Server, MaxPayload };
    caller.SealReceiving(Key("server-to-caller"));
    server.SealSending(Key("server-to-caller"));

    auto const payload = std::vector<std::byte>(9, std::byte { 0x5A });
    auto const reply = Wire::EncodeReply(Wire::Status::Ok, payload);
    REQUIRE(Written(server, reply) == reply.size());
    CHECK(ReadAvailable(caller) == reply);
}

TEST_CASE("A frame whose tag nobody computed ends the connection and says why", "[protocol][seal]")
{
    // What a relay can send: a well-formed frame behind a tag it made up, having no session key.
    auto pair = InMemorySocketPair::Create();
    auto const server = SealedServer(std::move(pair.server), "caller-to-server");

    REQUIRE(Written(*pair.client, ForgedRequest(RequestFrame())) > 0);
    auto const read = ReadAvailable(*server);
    REQUIRE_FALSE(read.has_value());
    CHECK(server->Fault() == SealFault::BadTag);
    CHECK(read.error().context == DescribeSealFault(SealFault::BadTag));

    // And it stays ended: a reader asking again meets the same fault, never the next bytes.
    CHECK_FALSE(ReadAvailable(*server).has_value());
}

TEST_CASE("A sealed frame replayed inside its session fails its tag", "[protocol][seal]")
{
    // The genuine bytes, captured once and sent twice. The first is the sender's and is handed
    // out; the second is checked at the NEXT position and refused.
    FrameSealer sealer { Key("caller-to-server") };
    auto const genuine = SealedRequest(sealer, RequestFrame());

    auto pair = InMemorySocketPair::Create();
    auto const server = SealedServer(std::move(pair.server), "caller-to-server");
    REQUIRE(Written(*pair.client, genuine) == genuine.size());
    CHECK(ReadAvailable(*server) == RequestFrame());

    REQUIRE(Written(*pair.client, genuine) == genuine.size());
    CHECK_FALSE(ReadAvailable(*server).has_value());
    CHECK(server->Fault() == SealFault::BadTag);
}

TEST_CASE("A frame sealed under the other direction's key is refused", "[protocol][seal]")
{
    // One key per direction: a reply reflected back at the server as if it were a request, or a
    // derivation that swapped the two, must not verify.
    FrameSealer wrongDirection { Key("server-to-caller") };
    auto pair = InMemorySocketPair::Create();
    auto const server = SealedServer(std::move(pair.server), "caller-to-server");
    REQUIRE(Written(*pair.client, SealedRequest(wrongDirection, RequestFrame())) > 0);
    CHECK_FALSE(ReadAvailable(*server).has_value());
    CHECK(server->Fault() == SealFault::BadTag);
}

TEST_CASE("Frames verified before a forged one are still handed out, and the forged one is not", "[protocol][seal]")
{
    // The first frame is the sender's own, so it is delivered; what follows it in the same read
    // is a relay's, and the reader meets the fault on its next read rather than the frame.
    FrameSealer sealer { Key("caller-to-server") };
    auto bytes = SealedRequest(sealer, RequestFrame("first"));
    auto const injected = ForgedRequest(RequestFrame("injected"));
    bytes.insert(bytes.end(), injected.begin(), injected.end());

    auto pair = InMemorySocketPair::Create();
    auto const server = SealedServer(std::move(pair.server), "caller-to-server");
    REQUIRE(Written(*pair.client, bytes) == bytes.size());

    CHECK(ReadAvailable(*server) == RequestFrame("first"));
    CHECK_FALSE(ReadAvailable(*server).has_value());
    CHECK(server->Fault() == SealFault::BadTag);
}

TEST_CASE("A sealed frame larger than the end holds is refused before its tag could be checked", "[protocol][seal]")
{
    // The tag follows the payload, so a declared length is a promise to buffer that much first;
    // a ceiling is what stops a peer that cannot seal from making this end hold it.
    auto pair = InMemorySocketPair::Create();
    auto const server = SealedServer(std::move(pair.server), "caller-to-server");
    auto const oversized = RequestFrame(std::string(MaxPayload + 1, 'k'));
    REQUIRE(Written(*pair.client, std::span<std::byte const> { oversized }.first(Wire::RequestHeaderSize)) > 0);

    CHECK_FALSE(ReadAvailable(*server).has_value());
    CHECK(server->Fault() == SealFault::Oversized);
}

TEST_CASE("Bytes that are not a frame end a sealed connection as unframed", "[protocol][seal]")
{
    auto pair = InMemorySocketPair::Create();
    auto const server = SealedServer(std::move(pair.server), "caller-to-server");
    auto const noise = std::vector<std::byte>(Wire::RequestHeaderSize, std::byte { 0 });
    REQUIRE(Written(*pair.client, noise) == noise.size());

    CHECK_FALSE(ReadAvailable(*server).has_value());
    CHECK(server->Fault() == SealFault::Unframed);
}

TEST_CASE("A peer that stops sending mid-frame has said goodbye, and the half frame is never handed out", "[protocol][seal]")
{
    // EOF means the peer finished sending. What it left unfinished was never verified, so the
    // reader sees the goodbye -- and no fault, because nothing was forged.
    FrameSealer sealer { Key("caller-to-server") };
    auto const genuine = SealedRequest(sealer, RequestFrame());

    auto pair = InMemorySocketPair::Create();
    auto const server = SealedServer(std::move(pair.server), "caller-to-server");
    REQUIRE(Written(*pair.client, std::span<std::byte const> { genuine }.first(genuine.size() - 1)) > 0);
    pair.client->shutdownWrite();

    auto const read = ReadAvailable(*server);
    REQUIRE(read.has_value());
    CHECK(read->empty());
    CHECK_FALSE(server->Fault().has_value());
}

TEST_CASE("Every seal fault says what happened", "[protocol][seal]")
{
    for (auto const fault: { SealFault::BadTag, SealFault::Oversized, SealFault::Unframed })
        CHECK_FALSE(DescribeSealFault(fault).empty());
}

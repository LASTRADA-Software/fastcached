// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/Bytes.hpp>
#include <FastCache/Net/Framing/LineReader.hpp>
#include <FastCache/Net/InMemoryTransport.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace
{

void PushAndClose(FastCache::ISocket& socket, std::string_view data)
{
    auto const bytes = FastCache::AsBytes(data);
    auto const result = FastCache::SyncRun(
        [](FastCache::ISocket& s, std::span<std::byte const> b) -> FastCache::Task<bool> {
            auto const r = co_await s.Write(b);
            co_return r.has_value();
        }(socket, bytes));
    REQUIRE(result);
    socket.Close();
}

} // namespace

TEST_CASE("ByteReader extracts a single line", "[net][linereader]")
{
    auto pair = FastCache::InMemorySocketPair::Create();
    PushAndClose(*pair.client, "hello\r\n");

    FastCache::ByteReader reader { *pair.server, /*maxLineBytes*/ 1024, /*maxPayloadBytes*/ 1024 };
    auto const line = FastCache::SyncRun(reader.ReadLine());
    REQUIRE(line.has_value());
    REQUIRE(*line == "hello");
}

TEST_CASE("ByteReader handles a payload that follows a line", "[net][linereader]")
{
    auto pair = FastCache::InMemorySocketPair::Create();
    PushAndClose(*pair.client, "set foo 0 0 5\r\nhello\r\n");

    FastCache::ByteReader reader { *pair.server, 1024, 1024 };
    auto const line = FastCache::SyncRun(reader.ReadLine());
    REQUIRE(line.has_value());
    REQUIRE(*line == "set foo 0 0 5");

    auto const body = FastCache::SyncRun(reader.ReadExactly(5));
    REQUIRE(body.has_value());
    REQUIRE(body->size() == 5);
    REQUIRE(static_cast<char>((*body)[0]) == 'h');

    // Trailing \r\n still in buffer.
    auto const tail = FastCache::SyncRun(reader.ReadLine());
    REQUIRE(tail.has_value());
    REQUIRE(tail->empty());
}

TEST_CASE("ByteReader rejects lines past the cap", "[net][linereader]")
{
    auto pair = FastCache::InMemorySocketPair::Create();
    std::string oversized(64, 'A');
    oversized += "\r\n";
    PushAndClose(*pair.client, oversized);

    FastCache::ByteReader reader { *pair.server, /*maxLineBytes*/ 16, /*maxPayloadBytes*/ 1024 };
    auto const line = FastCache::SyncRun(reader.ReadLine());
    REQUIRE_FALSE(line.has_value());
    REQUIRE(line.error().code == FastCache::ProtocolErrorCode::LineTooLong);
}

TEST_CASE("ByteReader reports truncation on EOF before line", "[net][linereader]")
{
    auto pair = FastCache::InMemorySocketPair::Create();
    pair.client->Close();

    FastCache::ByteReader reader { *pair.server, 1024, 1024 };
    auto const line = FastCache::SyncRun(reader.ReadLine());
    REQUIRE_FALSE(line.has_value());
    REQUIRE(line.error().code == FastCache::ProtocolErrorCode::Truncated);
}

TEST_CASE("ByteReader rejects oversized payload requests", "[net][linereader]")
{
    auto pair = FastCache::InMemorySocketPair::Create();
    PushAndClose(*pair.client, "anything");

    FastCache::ByteReader reader { *pair.server, 1024, /*maxPayloadBytes*/ 4 };
    auto const body = FastCache::SyncRun(reader.ReadExactly(100));
    REQUIRE_FALSE(body.has_value());
    REQUIRE(body.error().code == FastCache::ProtocolErrorCode::PayloadTooLarge);
}
